#include "diag/core/HalContext.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "isml_diag_uapi.h"

extern "C" {
#include <atlas_csr_sw/src/h/TOP.h>
}

static_assert(HOST_DMA_MAX_SIZE_BYTES == ISML_DIAG_HOST_DMA_MAX_SIZE_BYTES,
              "HAL and kernel UAPI host DMA limits must match");

namespace {

constexpr std::array<uint32_t, 3> PROBE_BAR_INDICES = {0, 2, 4};

// PCI configuration-space Command register and the bits required before the
// CPU maps BARs or the device initiates DMA. Values come from the PCI spec.
constexpr off_t PCI_COMMAND_OFFSET = 0x04;
constexpr uint16_t PCI_COMMAND_MEMORY_SPACE = 0x0002;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 0x0004;
constexpr uint16_t PCI_COMMAND_REQUIRED_BITS =
    PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;

std::string bar_pair_name(uint32_t bar_index)
{
    return "bar" + std::to_string(bar_index) + std::to_string(bar_index + 1);
}

uint64_t expected_bar_size(uint32_t bar_index)
{
    constexpr uint64_t mib = 1024ull * 1024ull;
    constexpr uint64_t gib = 1024ull * mib;

    if (bar_index == 0) {
        return 1ull * gib;
    }
    if (bar_index == 2 || bar_index == 4) {
        return 512ull * gib;
    }
    return 0;
}

std::vector<std::string> bar_layout(uint32_t bar_index)
{
    if (bar_index == 0) {
        return {
            "2MiB MSI-X",
            "126MiB indirect CSR",
            "128MiB debug",
            "256MiB doorbell",
            "512MiB RCF"
        };
    }
    if (bar_index == 2) {
        return {
            "256GiB RDMA",
            "256GiB DF",
            "8 apertures: aperture0 256MiB VMEM/SMEM readonly, aperture1-7 configurable DMEM"
        };
    }
    if (bar_index == 4) {
        return {
            "512GiB DF",
            "8 configurable DMEM apertures"
        };
    }
    return {};
}

BarMapping make_bar_mapping(uint32_t bar_index)
{
    BarMapping bar;
    bar.bar_index = bar_index;
    bar.name = bar_pair_name(bar_index);
    bar.expected_size = expected_bar_size(bar_index);
    bar.layout = bar_layout(bar_index);
    return bar;
}

uint64_t fake_bar_device_base_for_bdf(const std::string& bdf, uint32_t bar_index)
{
    // Dry-run BAR device-visible base. Real backends should discover this from:
    //   pHal: /sys/bus/pci/devices/<bdf>/resourceN start address or VFIO region info.
    //   iHal: vendor HAL device BAR metadata, for example dev->bars[N].baseAddr.
    // This is the address a device DMA descriptor should use, not the CPU
    // virtual pointer returned by mmap/VFIO mmap.
    uint64_t device_base = 0;
    if (bdf == "0000:19:00.0" || bdf == "0000:65:00.0") {
        device_base = 0x80000000;
    } else if (bdf == "0000:ca:00.0") {
        device_base = 0x90000000;
    }
    return device_base == 0 ? 0 : device_base + static_cast<uint64_t>(bar_index) * 0x100000;
}

std::string setpci_enable_command(const std::string& bdf)
{
    return "sudo setpci -s " + bdf + " COMMAND=0006:0006";
}

bool is_permission_error(int error_code)
{
    return error_code == EACCES || error_code == EPERM;
}

// Ensure MMIO and bus mastering are enabled before BAR mmap.
// When sysfs config writes are blocked, report the exact setpci command
// needed to enable PCI COMMAND.Mem+ and BusMaster for this device.
bool ensure_pci_mmio_enabled(const std::string& bdf, std::string& error)
{
    const auto config_path = "/sys/bus/pci/devices/" + bdf + "/config";

    int fd = open(config_path.c_str(), O_RDONLY);
    if (fd < 0) {
        auto open_errno = errno;
        error = "open PCI config failed for " + bdf + ": " + std::strerror(open_errno);
        if (is_permission_error(open_errno)) {
            error += "; run: " + setpci_enable_command(bdf);
        }
        return false;
    }

    uint16_t command = 0;
    auto bytes = pread(fd, &command, sizeof(command), PCI_COMMAND_OFFSET);
    auto read_errno = errno;
    close(fd);

    if (bytes != static_cast<ssize_t>(sizeof(command))) {
        error = "read PCI COMMAND failed for " + bdf + ": " + std::strerror(read_errno);
        if (is_permission_error(read_errno)) {
            error += "; run: " + setpci_enable_command(bdf);
        }
        return false;
    }

    if ((command & PCI_COMMAND_REQUIRED_BITS) == PCI_COMMAND_REQUIRED_BITS) {
        return true;
    }

    fd = open(config_path.c_str(), O_RDWR);
    if (fd < 0) {
        auto open_errno = errno;
        error = "PCI COMMAND.Mem+ or BusMaster is disabled for " + bdf +
                ", and enabling it failed: " + std::strerror(open_errno) +
                "; run: " + setpci_enable_command(bdf);
        return false;
    }

    const uint16_t enabled_command = command | PCI_COMMAND_REQUIRED_BITS;
    bytes = pwrite(fd, &enabled_command, sizeof(enabled_command), PCI_COMMAND_OFFSET);
    auto write_errno = errno;
    close(fd);

    if (bytes != static_cast<ssize_t>(sizeof(enabled_command))) {
        error = "enable PCI COMMAND.Mem+ and BusMaster failed for " + bdf +
                ": " + std::strerror(write_errno) +
                "; run: " + setpci_enable_command(bdf);
        return false;
    }

    return true;
}

bool read_hex_u16(const std::filesystem::path& path, uint16_t& value)
{
    std::ifstream input(path);
    std::string text;
    if (!std::getline(input, text)) {
        return false;
    }

    try {
        value = static_cast<uint16_t>(std::stoul(text, nullptr, 0));
        return true;
    } catch (...) {
        return false;
    }
}

bool read_resource_info(const std::string& bdf,
                        uint32_t bar_index,
                        uint64_t& start,
                        uint64_t& size_bytes)
{
    std::ifstream input("/sys/bus/pci/devices/" + bdf + "/resource");
    std::string line;
    for (uint32_t i = 0; i <= bar_index; ++i) {
        if (!std::getline(input, line)) {
            return false;
        }
    }

    uint64_t end = 0;
    uint64_t flags = 0;
    std::istringstream stream(line);
    if (!(stream >> std::hex >> start >> end >> flags)) {
        return false;
    }
    if (end < start) {
        return false;
    }
    if (start == 0 && end == 0 && flags == 0) {
        return false;
    }

    size_bytes = end - start + 1;
    return size_bytes != 0;
}

// Open the isml_diag character device bound to bdf. This function only finds
// and opens the matching device node; DMA allocation and mmap are performed by
// HalContext::host_mem_alloc() after it returns the file descriptor.
int open_device(const std::string& bdf)
{
    unsigned int domain = 0;
    unsigned int bus = 0;
    unsigned int device = 0;
    unsigned int function = 0;
    if (std::sscanf(bdf.c_str(), "%x:%x:%x.%x",
                    &domain, &bus, &device, &function) != 4) {
        return -1;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
        if (ec) {
            break;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind("isml_diag", 0) != 0) {
            continue;
        }

        int fd = open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        isml_diag_device_info info = {};
        if (ioctl(fd, ISML_DIAG_IOCTL_GET_DEVICE_INFO, &info) == 0 &&
            info.abi_version == ISML_DIAG_ABI_VERSION &&
            info.domain == domain &&
            info.bus == bus &&
            (info.devfn >> 3) == device &&
            (info.devfn & 0x7) == function) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

}

std::string to_string(HalType type)
{
    switch (type) {
    case HalType::pHal:
        return "pHal";
    case HalType::iHal:
        return "iHal";
    }
    return "unknown";
}

DmaBuffer::DmaBuffer(HalContext* alloc_ctx,
                     void* cpu_base,
                     uint64_t size_bytes,
                     uint64_t device_addr,
                     uint64_t handle,
                     int backend_fd,
                     std::string addr_kind)
    : alloc_ctx_(alloc_ctx),
      cpu_base_(cpu_base),
      device_addr_(device_addr),
      size_(size_bytes),
      handle_(handle),
      backend_fd_(backend_fd),
      addr_kind_(std::move(addr_kind))
{
}

DmaBuffer::~DmaBuffer()
{
    release();
}

DmaBuffer::DmaBuffer(DmaBuffer&& other) noexcept
    : alloc_ctx_(other.alloc_ctx_),
      cpu_base_(other.cpu_base_),
      device_addr_(other.device_addr_),
      size_(other.size_),
      handle_(other.handle_),
      backend_fd_(other.backend_fd_),
      addr_kind_(std::move(other.addr_kind_))
{
    other.alloc_ctx_ = nullptr;
    other.cpu_base_ = nullptr;
    other.device_addr_ = 0;
    other.size_ = 0;
    other.handle_ = 0;
    other.backend_fd_ = -1;
}

DmaBuffer& DmaBuffer::operator=(DmaBuffer&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    release();

    alloc_ctx_ = other.alloc_ctx_;
    cpu_base_ = other.cpu_base_;
    device_addr_ = other.device_addr_;
    size_ = other.size_;
    handle_ = other.handle_;
    backend_fd_ = other.backend_fd_;
    addr_kind_ = std::move(other.addr_kind_);

    other.alloc_ctx_ = nullptr;
    other.cpu_base_ = nullptr;
    other.device_addr_ = 0;
    other.size_ = 0;
    other.handle_ = 0;
    other.backend_fd_ = -1;

    return *this;
}

void DmaBuffer::release()
{
    if (alloc_ctx_ == nullptr) {
        return;
    }
    alloc_ctx_->free_host_dma_buffer(*this);
}

HalContext::HalContext(HalType type)
    : type_(type)
{
}

void HalContext::bind_device(const DeviceContext& ctx, Logger* logger)
{
    // A HalContext represents one currently bound physical device. Rebinding
    // releases contexts tied to the previous BAR mapping before replacing it.
    phal_context_ = {};
    aperture_phal_context_ = {};
    active_device_ = ctx;
    active_device_.logger = logger != nullptr ? logger : ctx.logger;
    active_memory_regions_ = ctx.memory_regions;
    active_memory_ = ctx.memory;
    if (active_memory_ == nullptr) {
        active_memory_ = std::make_shared<DeviceMemoryState>();
    }
    active_device_.memory = active_memory_;
    active_device_.memory_regions = active_memory_regions_;
    device_bound_ = true;
}

HalContext::TestcaseScope HalContext::begin_testcase(std::string* error)
{
    if (error != nullptr) error->clear();
    if (!device_bound_) {
        if (error != nullptr) *error = "HAL is not bound to a device";
        return {};
    }
    if (phal_context_ || aperture_phal_context_) {
        if (error != nullptr) *error = "a testcase PHAL context is already active";
        return {};
    }

    std::string init_error;
    const auto project = phal_project_from_tpu_type(active_device_.tpu_type);
    phal_context_ = phal_bridge_.create_context(active_device_, project, &init_error);
    if (!phal_context_) {
        if (error != nullptr) *error = "root PHAL initialization failed: " + init_error;
        return {};
    }

    init_error.clear();
    aperture_phal_context_ =
        phal_bridge_.create_context(active_device_, project, &init_error);
    if (!aperture_phal_context_) {
        phal_context_ = {};
        if (error != nullptr) {
            *error = "aperture PHAL initialization failed: " + init_error;
        }
        return {};
    }
    return TestcaseScope(this);
}

HalContext::TestcaseScope::~TestcaseScope()
{
    if (owner_ != nullptr) {
        owner_->phal_context_ = {};
        owner_->aperture_phal_context_ = {};
    }
}

HalContext::TestcaseScope::TestcaseScope(TestcaseScope&& other) noexcept
    : owner_(other.owner_)
{
    other.owner_ = nullptr;
}

HalContext::TestcaseScope& HalContext::TestcaseScope::operator=(TestcaseScope&& other) noexcept
{
    if (this != &other) {
        if (owner_ != nullptr) {
            owner_->phal_context_ = {};
            owner_->aperture_phal_context_ = {};
        }
        owner_ = other.owner_;
        other.owner_ = nullptr;
    }
    return *this;
}

const DeviceContext& HalContext::device_context() const
{
    return active_device_;
}

TestStatus HalContext::with_aperture_context(
    const std::function<TestStatus(phal_ctx_t*)>& operation,
    std::string* error)
{
    if (error != nullptr) error->clear();
    if (!operation) {
        if (error != nullptr) *error = "aperture operation callback is empty";
        return PHAL_STATUS_INVALID;
    }
    if (!device_bound_ || !aperture_phal_context_) {
        if (error != nullptr) *error = "aperture PHAL context is not initialized";
        return PHAL_STATUS_ERROR;
    }

    std::lock_guard<std::mutex> lock(active_memory_->aperture_mutex);
    auto* phal = aperture_phal_context_.get();
    phal_block_ctx_enter(phal, TOP_U_PCIE_0_OFFSET);
    TestStatus status = PHAL_STATUS_ERROR;
    try {
        status = operation(phal);
    } catch (...) {
        phal_block_ctx_exit(phal, TOP_U_PCIE_0_OFFSET);
        phal->env.base = PhalBridge::CHIP_RCF_BASE;
        throw;
    }
    phal_block_ctx_exit(phal, TOP_U_PCIE_0_OFFSET);
    // Keep the reusable context at the chip root even if a callback left a
    // nested block active after returning.
    phal->env.base = PhalBridge::CHIP_RCF_BASE;
    return status;
}

std::vector<DeviceContext> HalContext::scan_pci_devices() const
{
    if (type_ == HalType::iHal) {
        std::vector<DeviceContext> devices;
        const std::filesystem::path sysfs_devices("/sys/bus/pci/devices");

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(sysfs_devices, ec)) {
            if (ec) {
                break;
            }

            uint16_t vendor_id = 0;
            uint16_t device_id = 0;
            if (!read_hex_u16(entry.path() / "vendor", vendor_id) ||
                !read_hex_u16(entry.path() / "device", device_id)) {
                continue;
            }

            DeviceContext ctx;
            ctx.bdf = entry.path().filename().string();
            ctx.vendor_id = vendor_id;
            ctx.device_id = device_id;
            devices.push_back(ctx);
        }
        return devices;
    }

    // The pHal backend uses synthetic inventory for local dry-run testing until
    // platform discovery is connected to the vendor API.
    DeviceContext tpu0;
    tpu0.bdf = "0000:19:00.0";
    tpu0.vendor_id = 0x16c3;
    tpu0.device_id = 0xabcd;

    DeviceContext tpu1;
    tpu1.bdf = "0000:ca:00.0";
    tpu1.vendor_id = 0x16c3;
    tpu1.device_id = 0xabcd;

    DeviceContext unrelated_device;
    unrelated_device.bdf = "0000:17:00.0";
    unrelated_device.vendor_id = 0xffff;
    unrelated_device.device_id = 0xffff;

    return {tpu0, tpu1, unrelated_device};
}

DeviceContext HalContext::mmap_bar_space(DeviceContext ctx)
{
    ctx.bar_mappings.clear();

    if (type_ == HalType::iHal) {
        std::string command_error;
        if (!ensure_pci_mmio_enabled(ctx.bdf, command_error)) {
            for (auto bar_index : PROBE_BAR_INDICES) {
                auto bar = make_bar_mapping(bar_index);
                bar.error = command_error;
                ctx.bar_mappings.push_back(std::move(bar));
            }
            return ctx;
        }

        for (auto bar_index : PROBE_BAR_INDICES) {
            auto bar = make_bar_mapping(bar_index);

            uint64_t bar_start = 0;
            uint64_t bar_size = 0;
            if (!read_resource_info(ctx.bdf, bar_index, bar_start, bar_size)) {
                bar.error = "resource" + std::to_string(bar_index) + " is not present";
                ctx.bar_mappings.push_back(std::move(bar));
                continue;
            }

            auto resource = "/sys/bus/pci/devices/" + ctx.bdf + "/resource" +
                            std::to_string(bar_index);
            int fd = open(resource.c_str(), O_RDWR | O_SYNC);
            int prot = PROT_READ | PROT_WRITE;
            if (fd < 0) {
                fd = open(resource.c_str(), O_RDONLY | O_SYNC);
                prot = PROT_READ;
            }
            if (fd < 0) {
                bar.device_base = bar_start;
                bar.resource_size = bar_size;
                bar.error = "open resource" + std::to_string(bar_index) +
                            " failed: " + std::strerror(errno);
                ctx.bar_mappings.push_back(std::move(bar));
                continue;
            }

            const uint64_t map_size = bar_size;
            void* mapped = mmap(nullptr,
                                static_cast<size_t>(map_size),
                                prot,
                                MAP_SHARED,
                                fd,
                                0);
            auto mmap_errno = errno;
            close(fd);

            bar.device_base = bar_start;
            bar.resource_size = bar_size;
            bar.mapped_size = map_size;
            if (mapped == MAP_FAILED) {
                bar.error = "mmap resource" + std::to_string(bar_index) +
                            " failed: " + std::strerror(mmap_errno);
                ctx.bar_mappings.push_back(std::move(bar));
                continue;
            }

            bar.mapped_base = mapped;
            bar.mapped = true;
            bar.writable = (prot & PROT_WRITE) != 0;
            mapped_bar_mappings_.push_back({mapped, map_size});

            ctx.bar_mappings.push_back(std::move(bar));
        }
        return ctx;
    }

    // Synthetic mappings keep local tests independent of physical hardware.
    for (auto bar_index : PROBE_BAR_INDICES) {
        mapped_bar_storage_.push_back(
            std::make_unique<std::vector<uint8_t>>(DRYRUN_BAR_WINDOW_SIZE, 0));

        auto bar = make_bar_mapping(bar_index);
        bar.mapped_base = mapped_bar_storage_.back()->data();
        bar.device_base = fake_bar_device_base_for_bdf(ctx.bdf, bar_index);
        bar.resource_size = bar.expected_size;
        bar.mapped_size = DRYRUN_BAR_WINDOW_SIZE;
        bar.mapped = true;
        bar.writable = true;

        ctx.bar_mappings.push_back(std::move(bar));
    }
    return ctx;
}

DmaBuffer HalContext::host_mem_alloc(uint64_t size_bytes)
{
    if (size_bytes == 0 ||
        size_bytes > HOST_DMA_MAX_SIZE_BYTES ||
        size_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return {};
    }

    if (type_ == HalType::iHal) {
        if (!device_bound_) {
            return {};
        }
        int fd = open_device(active_device_.bdf);
        if (fd < 0) {
            return {};
        }

        isml_diag_dma_alloc request = {};
        request.size = size_bytes;
        if (ioctl(fd, ISML_DIAG_IOCTL_DMA_ALLOC, &request) != 0) {
            close(fd);
            return {};
        }

        void* cpu_base = mmap(nullptr,
                              static_cast<size_t>(request.size),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,
                              fd,
                              static_cast<off_t>(request.mmap_offset));
        if (cpu_base == MAP_FAILED) {
            isml_diag_dma_free release = {request.handle};
            ioctl(fd, ISML_DIAG_IOCTL_DMA_FREE, &release);
            close(fd);
            return {};
        }

        return DmaBuffer(this,
                         cpu_base,
                         request.size,
                         request.device_addr,
                         request.handle,
                         fd,
                         "dma_addr");
    }

    // pHal host-DMA allocation must come from the ProtonHal VFIO driver so its
    // device address is valid in that driver's IOMMU domain. That integration
    // is intentionally unimplemented; do not fabricate a DMA-capable address.
    return {};
}

void HalContext::free_host_dma_buffer(DmaBuffer& buffer)
{
    if (buffer.backend_fd_ >= 0) {
        if (buffer.cpu_base_ != nullptr && buffer.size_ != 0) {
            munmap(buffer.cpu_base_, static_cast<size_t>(buffer.size_));
        }
        isml_diag_dma_free request = {buffer.handle_};
        ioctl(buffer.backend_fd_, ISML_DIAG_IOCTL_DMA_FREE, &request);
        close(buffer.backend_fd_);
    }
    buffer.alloc_ctx_ = nullptr;
    buffer.cpu_base_ = nullptr;
    buffer.device_addr_ = 0;
    buffer.size_ = 0;
    buffer.handle_ = 0;
    buffer.backend_fd_ = -1;
    buffer.addr_kind_.clear();
}

void HalContext::clear_mappings()
{
    phal_context_ = {};
    aperture_phal_context_ = {};
    if (type_ == HalType::iHal) {
        for (const auto& mapping : mapped_bar_mappings_) {
            if (mapping.first != nullptr && mapping.second != 0) {
                munmap(mapping.first, static_cast<size_t>(mapping.second));
            }
        }
        mapped_bar_mappings_.clear();
    }
    mapped_bar_storage_.clear();
}

void HalContext::clear()
{
    clear_mappings();
    active_device_ = {};
    active_memory_regions_ = {};
    active_memory_ = std::make_shared<DeviceMemoryState>();
    device_bound_ = false;
}
