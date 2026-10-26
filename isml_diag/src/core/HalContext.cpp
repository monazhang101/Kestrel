#include "diag/core/HalContext.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0x20
#endif
#endif
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

#include "isml_diag_uapi.h"

extern "C" {
#include <atlas_csr_sw/src/h/TOP.h>
}

static_assert(HOST_DMA_MAX_SIZE_BYTES == ISML_DIAG_HOST_DMA_MAX_SIZE_BYTES,
              "HAL and kernel UAPI host DMA limits must match");

namespace {

constexpr std::array<uint32_t, 3> PROBE_BAR_INDICES = {0, 2, 4};
constexpr const char* ISML_DRIVER_DEVICE_PREFIX = "isml_drv";
constexpr uint64_t HUGEPAGE_2MB_SIZE = 2ull * 1024ull * 1024ull;
constexpr const char* HUGEPAGE_2MB_FREE_PATH =
    "/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages";
constexpr const char* HUGEPAGE_2MB_TOTAL_PATH =
    "/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages";
constexpr uint64_t PAGEMAP_PRESENT = UINT64_C(1) << 63;
constexpr uint64_t PAGEMAP_PFN_MASK = (UINT64_C(1) << 55) - 1;

// Standard PCI/PCIe configuration-space fields used by the host access path.
// Type-1 Bridge Control bit 6 is the standard Secondary Bus Reset control.
constexpr off_t PCI_COMMAND_OFFSET = 0x04;
constexpr uint16_t PCI_COMMAND_MEMORY_SPACE = 0x0002;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 0x0004;
constexpr uint16_t PCI_COMMAND_REQUIRED_BITS =
    PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
constexpr off_t PCI_HEADER_TYPE_OFFSET = 0x0e;             // Common Header Type.
constexpr uint8_t PCI_HEADER_TYPE_MASK = 0x7f;             // Ignore multifunction bit 7.
constexpr uint8_t PCI_HEADER_TYPE_BRIDGE = 0x01;           // PCI-to-PCI bridge header.
constexpr off_t PCI_BRIDGE_CONTROL_OFFSET = 0x3e;          // Type-1 Bridge Control.
constexpr uint16_t PCI_BRIDGE_CONTROL_BUS_RESET = 0x0040;  // Secondary Bus Reset, bit 6.

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

std::string setpci_enable_command(const std::string& bdf)
{
    return "sudo setpci -s " + bdf + " COMMAND=0006:0006";
}

bool upstream_bridge_config(const std::string& endpoint_bdf,
                            std::filesystem::path& config_path,
                            std::string& bridge_bdf,
                            std::string& error)
{
    // The endpoint's parent in sysfs is its immediate upstream PCI bridge.
    std::error_code filesystem_error;
    const auto endpoint_path = std::filesystem::canonical(
        std::filesystem::path("/sys/bus/pci/devices") / endpoint_bdf,
        filesystem_error);
    if (filesystem_error) {
        error = "resolve PCI device failed for " + endpoint_bdf + ": " +
                filesystem_error.message();
        return false;
    }

    const auto bridge_path = endpoint_path.parent_path();
    config_path = bridge_path / "config";
    if (bridge_path.empty() ||
        !std::filesystem::exists(config_path, filesystem_error) ||
        filesystem_error) {
        error = "upstream PCI bridge config is unavailable for " + endpoint_bdf;
        return false;
    }

    bridge_bdf = bridge_path.filename().string();
    return true;
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

// Open the isml_drv character device bound to bdf. This function only finds
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
        if (name.rfind(ISML_DRIVER_DEVICE_PREFIX, 0) != 0) {
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

bool read_first_line(const std::filesystem::path& path,
                     std::string& value,
                     std::string& error)
{
    std::ifstream input(path);
    if (!input || !std::getline(input, value)) {
        error = "read " + path.string() + " failed";
        return false;
    }
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
        value.pop_back();
    }
    return true;
}

bool read_uint64(const std::filesystem::path& path,
                 uint64_t& value,
                 std::string& error)
{
    std::string text;
    if (!read_first_line(path, text, error)) return false;
    try {
        size_t parsed = 0;
        value = std::stoull(text, &parsed, 10);
        if (parsed != text.size()) throw std::invalid_argument("trailing characters");
    } catch (const std::exception&) {
        error = "parse " + path.string() + " failed: " + text;
        return false;
    }
    return true;
}

bool ensure_free_2mb_hugepage(Logger* logger, std::string& error)
{
    uint64_t free_pages = 0;
    if (!read_uint64(HUGEPAGE_2MB_FREE_PATH, free_pages, error)) return false;
    if (free_pages != 0) return true;

    uint64_t total_pages = 0;
    if (!read_uint64(HUGEPAGE_2MB_TOTAL_PATH, total_pages, error)) return false;
    if (total_pages == std::numeric_limits<uint64_t>::max()) {
        error = "2 MiB HugeTLB page count overflow";
        return false;
    }

    const auto requested_total = std::to_string(total_pages + 1) + "\n";
    int fd = open(HUGEPAGE_2MB_TOTAL_PATH, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "open " + std::string(HUGEPAGE_2MB_TOTAL_PATH) +
                " failed: " + std::strerror(errno) +
                "; run as root or reserve one 2 MiB hugepage manually";
        return false;
    }
    errno = 0;
    const auto written = write(fd, requested_total.data(), requested_total.size());
    const auto write_errno = errno;
    close(fd);
    if (written != static_cast<ssize_t>(requested_total.size())) {
        error = "increase 2 MiB HugeTLB pool failed: " +
                std::string(std::strerror(write_errno)) +
                "; run: sudo sysctl -w vm.nr_hugepages=" +
                std::to_string(total_pages + 1);
        return false;
    }

    uint64_t actual_total = 0;
    if (!read_uint64(HUGEPAGE_2MB_TOTAL_PATH, actual_total, error)) return false;
    if (actual_total < total_pages + 1) {
        error = "kernel could not reserve another 2 MiB HugeTLB page; requested " +
                std::to_string(total_pages + 1) + ", actual " +
                std::to_string(actual_total);
        return false;
    }

    if (logger != nullptr) {
        logger->flow("2 MiB HugeTLB pool: nr_hugepages=" +
                     std::to_string(total_pages) + " -> " +
                     std::to_string(actual_total) +
                     "; reservation will be kept");
    }
    return true;
}

bool ensure_iommu_identity(const std::string& bdf,
                           Logger* logger,
                           std::string& error)
{
    const auto device_path = std::filesystem::path("/sys/bus/pci/devices") / bdf;
    std::error_code ec;
    const auto group_path = std::filesystem::canonical(device_path / "iommu_group", ec);
    if (ec) {
        error = "resolve IOMMU group failed for " + bdf + ": " + ec.message();
        return false;
    }

    const auto devices_path = group_path / "devices";
    size_t device_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(devices_path, ec)) {
        if (ec) break;
        ++device_count;
        std::error_code status_error;
        const bool has_driver =
            std::filesystem::exists(entry.path() / "driver", status_error);
        if (status_error) {
            error = "inspect driver binding for " +
                    entry.path().filename().string() + " failed: " +
                    status_error.message();
            return false;
        }
        if (has_driver) {
            error = "drv=none requires every device in IOMMU group " +
                    group_path.filename().string() + " to be unbound; " +
                    entry.path().filename().string() + " still has a driver";
            return false;
        }
    }
    if (ec || device_count == 0) {
        error = "enumerate IOMMU group devices failed for " + bdf;
        return false;
    }

    const auto type_path = group_path / "type";
    std::string original_type;
    if (!read_first_line(type_path, original_type, error)) return false;
    if (original_type == "identity") {
        if (logger != nullptr) {
            logger->flow("IOMMU group " + group_path.filename().string() +
                         " is already identity");
        }
        return true;
    }

    int fd = open(type_path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "open " + type_path.string() + " for identity switch failed: " +
                std::strerror(errno);
        return false;
    }
    constexpr char IDENTITY[] = "identity\n";
    errno = 0;
    const auto written = write(fd, IDENTITY, sizeof(IDENTITY) - 1);
    const auto write_errno = errno;
    close(fd);
    if (written != static_cast<ssize_t>(sizeof(IDENTITY) - 1)) {
        error = "set IOMMU group " + group_path.filename().string() +
                " type to identity failed: " + std::strerror(write_errno);
        if (write_errno == EBUSY) error += "; check for a bound group device";
        return false;
    }

    std::string actual_type;
    if (!read_first_line(type_path, actual_type, error)) return false;
    if (actual_type != "identity") {
        error = "IOMMU group " + group_path.filename().string() +
                " type readback is " + actual_type + ", expected identity";
        return false;
    }
    if (logger != nullptr) {
        logger->flow("IOMMU group " + group_path.filename().string() + ": " +
                     original_type + " -> identity; identity mode will be kept");
    }
    return true;
}

bool resolve_contiguous_physical(void* mapping,
                                 uint64_t mapped_size,
                                 uint64_t page_size,
                                 uint64_t& physical_address,
                                 std::string& error)
{
    if (mapping == nullptr || mapped_size == 0 || page_size == 0 ||
        mapped_size % page_size != 0) {
        error = "invalid pagemap range";
        return false;
    }

    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "open /proc/self/pagemap failed: " + std::string(std::strerror(errno));
        return false;
    }

    const auto base = reinterpret_cast<uintptr_t>(mapping);
    const uint64_t first_page = static_cast<uint64_t>(base) / page_size;
    const uint64_t page_count = mapped_size / page_size;
    uint64_t first_pfn = 0;
    for (uint64_t page = 0; page < page_count; ++page) {
        uint64_t entry = 0;
        const uint64_t index = first_page + page;
        if (index > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) /
                        sizeof(entry)) {
            close(fd);
            error = "pagemap offset is outside off_t range";
            return false;
        }
        const auto offset = static_cast<off_t>(index * sizeof(entry));
        const auto bytes = pread(fd, &entry, sizeof(entry), offset);
        if (bytes != static_cast<ssize_t>(sizeof(entry))) {
            const auto read_errno = errno;
            close(fd);
            error = "read /proc/self/pagemap failed: " +
                    std::string(std::strerror(read_errno));
            return false;
        }
        if ((entry & PAGEMAP_PRESENT) == 0) {
            close(fd);
            error = "host DMA page is not present";
            return false;
        }
        const uint64_t pfn = entry & PAGEMAP_PFN_MASK;
        if (pfn == 0) {
            close(fd);
            error = "pagemap PFN is unavailable; run with CAP_SYS_ADMIN/root";
            return false;
        }
        if (page == 0) {
            first_pfn = pfn;
        } else if (pfn != first_pfn + page) {
            close(fd);
            error = "host DMA pages are not physically contiguous";
            return false;
        }
    }
    close(fd);

    if (first_pfn > std::numeric_limits<uint64_t>::max() / page_size) {
        error = "host DMA physical address overflows uint64";
        return false;
    }
    physical_address = first_pfn * page_size;
    return true;
}

}

DmaBuffer::DmaBuffer(HalContext* alloc_ctx,
                     void* cpu_base,
                     uint64_t size_bytes,
                     uint64_t device_addr,
                     uint64_t handle,
                     int backend_fd,
                     uint64_t mapped_size,
                     bool memory_locked,
                     std::string addr_kind)
    : alloc_ctx_(alloc_ctx),
      cpu_base_(cpu_base),
      device_addr_(device_addr),
      size_(size_bytes),
      handle_(handle),
      backend_fd_(backend_fd),
      mapped_size_(mapped_size),
      memory_locked_(memory_locked),
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
      mapped_size_(other.mapped_size_),
      memory_locked_(other.memory_locked_),
      addr_kind_(std::move(other.addr_kind_))
{
    other.alloc_ctx_ = nullptr;
    other.cpu_base_ = nullptr;
    other.device_addr_ = 0;
    other.size_ = 0;
    other.handle_ = 0;
    other.backend_fd_ = -1;
    other.mapped_size_ = 0;
    other.memory_locked_ = false;
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
    mapped_size_ = other.mapped_size_;
    memory_locked_ = other.memory_locked_;
    addr_kind_ = std::move(other.addr_kind_);

    other.alloc_ctx_ = nullptr;
    other.cpu_base_ = nullptr;
    other.device_addr_ = 0;
    other.size_ = 0;
    other.handle_ = 0;
    other.backend_fd_ = -1;
    other.mapped_size_ = 0;
    other.memory_locked_ = false;

    return *this;
}

void DmaBuffer::release()
{
    if (alloc_ctx_ == nullptr) {
        return;
    }
    alloc_ctx_->free_host_dma_buffer(*this);
}

HalContext::HalContext() = default;

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
    host_dma_mode_ = HostDmaMode::IsmlDrv;
    device_bound_ = true;
}

TestStatus HalContext::configure_host_dma(const std::string& driver)
{
    if (driver == "isml_drv") {
        host_dma_mode_ = HostDmaMode::IsmlDrv;
        return PHAL_STATUS_OK;
    }
    if (driver == "none") {
        host_dma_mode_ = HostDmaMode::None;
        return PHAL_STATUS_OK;
    }
    if (active_device_.logger != nullptr) {
        active_device_.logger->error(
            "invalid drv=" + driver + "; expected isml_drv or none");
    }
    return PHAL_STATUS_INVALID;
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

TestStatus HalContext::with_pcie_context(
    const std::function<TestStatus(phal_ctx_t*)>& operation,
    std::string* error)
{
    if (error != nullptr) error->clear();
    if (!operation) {
        if (error != nullptr) *error = "PCIe operation callback is empty";
        return PHAL_STATUS_INVALID;
    }
    if (!device_bound_ || !aperture_phal_context_) {
        if (error != nullptr) *error = "PCIe PHAL context is not initialized";
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

TestStatus HalContext::secondary_bus_reset(bool reset)
{
    const auto fail = [&](TestStatus status, const std::string& message) {
        if (active_device_.logger != nullptr) {
            active_device_.logger->error(message);
        }
        return status;
    };

    if (!device_bound_ || active_device_.bdf.empty()) {
        return fail(PHAL_STATUS_INVALID,
                    "secondary bus reset requires a bound PCI device");
    }
    // Resolve the selected TPU endpoint to its immediate upstream bridge.
    std::filesystem::path config_path;
    std::string bridge_bdf;
    std::string resolve_error;
    if (!upstream_bridge_config(active_device_.bdf,
                                config_path,
                                bridge_bdf,
                                resolve_error)) {
        return fail(PHAL_STATUS_ERROR, resolve_error);
    }

    // Bridge Control is accessed through the bridge's PCI config-space file.
    const int fd = open(config_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return fail(PHAL_STATUS_ERROR,
                    "open upstream PCI bridge config failed for " + bridge_bdf +
                    ": " + std::strerror(errno));
    }

    // Secondary Bus Reset is defined only in the Type-1 bridge header.
    uint8_t header_type = 0;
    auto bytes = pread(fd, &header_type, sizeof(header_type), PCI_HEADER_TYPE_OFFSET);
    if (bytes != static_cast<ssize_t>(sizeof(header_type))) {
        const auto read_error = errno;
        close(fd);
        return fail(PHAL_STATUS_ERROR,
                    "read PCI header type failed for " + bridge_bdf +
                    (bytes < 0 ? ": " + std::string(std::strerror(read_error))
                               : ": short read"));
    }
    if ((header_type & PCI_HEADER_TYPE_MASK) != PCI_HEADER_TYPE_BRIDGE) {
        close(fd);
        return fail(PHAL_STATUS_ERROR,
                    "upstream PCI device is not a Type-1 bridge: " + bridge_bdf);
    }

    // Preserve every Bridge Control field except the requested reset bit.
    uint16_t bridge_control = 0;
    bytes = pread(fd,
                  &bridge_control,
                  sizeof(bridge_control),
                  PCI_BRIDGE_CONTROL_OFFSET);
    if (bytes != static_cast<ssize_t>(sizeof(bridge_control))) {
        const auto read_error = errno;
        close(fd);
        return fail(PHAL_STATUS_ERROR,
                    "read PCI Bridge Control failed for " + bridge_bdf +
                    (bytes < 0 ? ": " + std::string(std::strerror(read_error))
                               : ": short read"));
    }

    const uint16_t requested = reset
        ? static_cast<uint16_t>(bridge_control | PCI_BRIDGE_CONTROL_BUS_RESET)
        : static_cast<uint16_t>(bridge_control & ~PCI_BRIDGE_CONTROL_BUS_RESET);
    // Write the requested reset state to the standard Bridge Control register.
    bytes = pwrite(fd, &requested, sizeof(requested), PCI_BRIDGE_CONTROL_OFFSET);
    if (bytes != static_cast<ssize_t>(sizeof(requested))) {
        const auto write_error = errno;
        close(fd);
        return fail(PHAL_STATUS_ERROR,
                    "write PCI Bridge Control failed for " + bridge_bdf +
                    (bytes < 0 ? ": " + std::string(std::strerror(write_error))
                               : ": short write"));
    }

    // Read the register back and verify that bit 6 reached the requested state.
    uint16_t verified = 0;
    bytes = pread(fd, &verified, sizeof(verified), PCI_BRIDGE_CONTROL_OFFSET);
    const auto verify_error = errno;
    close(fd);
    if (bytes != static_cast<ssize_t>(sizeof(verified))) {
        return fail(PHAL_STATUS_ERROR,
                    "verify PCI Bridge Control failed for " + bridge_bdf +
                    (bytes < 0 ? ": " + std::string(std::strerror(verify_error))
                               : ": short read"));
    }

    const bool observed_reset =
        (verified & PCI_BRIDGE_CONTROL_BUS_RESET) != 0;
    if (observed_reset != reset) {
        return fail(PHAL_STATUS_ERROR,
                    "Secondary Bus Reset readback mismatch for " + bridge_bdf);
    }

    if (active_device_.logger != nullptr) {
        active_device_.logger->flow(
            "Secondary Bus Reset " + std::string(reset ? "asserted" : "deasserted") +
            ": endpoint=" + active_device_.bdf + " bridge=" + bridge_bdf);
    }
    return PHAL_STATUS_OK;
}

std::vector<DeviceContext> HalContext::scan_pci_devices() const
{
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

DeviceContext HalContext::mmap_bar_space(DeviceContext ctx)
{
    ctx.bar_mappings.clear();

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

DmaBuffer HalContext::host_mem_alloc(uint64_t size_bytes)
{
    if (size_bytes == 0 ||
        size_bytes > HOST_DMA_MAX_SIZE_BYTES ||
        size_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return {};
    }

    if (!device_bound_) return {};

    if (host_dma_mode_ == HostDmaMode::None) {
        if (size_bytes > HOST_DMA_NO_DRIVER_MAX_SIZE_BYTES) {
            if (active_device_.logger != nullptr) {
                active_device_.logger->error(
                    "drv=none host DMA size exceeds 2 MiB: " +
                    std::to_string(size_bytes));
            }
            return {};
        }

        std::string error;
        if (!ensure_iommu_identity(active_device_.bdf,
                                   active_device_.logger, error)) {
            if (active_device_.logger != nullptr) {
                active_device_.logger->error(error);
            }
            return {};
        }

        const long page_size_value = sysconf(_SC_PAGESIZE);
        if (page_size_value <= 0) {
            if (active_device_.logger != nullptr) {
                active_device_.logger->error("read system page size failed");
            }
            return {};
        }
        const uint64_t page_size = static_cast<uint64_t>(page_size_value);
        if (page_size > HUGEPAGE_2MB_SIZE ||
            size_bytes > std::numeric_limits<uint64_t>::max() - (page_size - 1)) {
            if (active_device_.logger != nullptr) {
                active_device_.logger->error("unsupported system page size for drv=none");
            }
            return {};
        }

        void* cpu_base = MAP_FAILED;
        uint64_t mapped_size = 0;
        int hugepage_errno = 0;
        std::string hugepage_reservation_error;
        bool used_hugepage = false;
        if (size_bytes > page_size) {
            mapped_size = HUGEPAGE_2MB_SIZE;
            cpu_base = mmap(nullptr,
                            static_cast<size_t>(mapped_size),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS |
                                MAP_HUGETLB | MAP_HUGE_2MB,
                            -1,
                            0);
            if (cpu_base == MAP_FAILED) {
                hugepage_errno = errno;
                if (hugepage_errno == ENOMEM &&
                    ensure_free_2mb_hugepage(active_device_.logger,
                                             hugepage_reservation_error)) {
                    cpu_base = mmap(nullptr,
                                    static_cast<size_t>(mapped_size),
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS |
                                        MAP_HUGETLB | MAP_HUGE_2MB,
                                    -1,
                                    0);
                    if (cpu_base == MAP_FAILED) {
                        hugepage_errno = errno;
                    }
                }
                if (cpu_base == MAP_FAILED && active_device_.logger != nullptr) {
                    std::string message =
                        "2 MiB HugeTLB allocation failed; trying a " +
                        std::to_string(size_bytes) +
                        "-byte anonymous mapping: " +
                        std::strerror(hugepage_errno);
                    if (!hugepage_reservation_error.empty()) {
                        message += "; automatic reservation failed: " +
                                   hugepage_reservation_error;
                    }
                    active_device_.logger->flow(message);
                }
            }
            if (cpu_base != MAP_FAILED) {
                used_hugepage = true;
            }
        }

        if (cpu_base == MAP_FAILED) {
            mapped_size = (size_bytes + page_size - 1) & ~(page_size - 1);
            cpu_base = mmap(nullptr,
                            static_cast<size_t>(mapped_size),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            -1,
                            0);
        }
        if (cpu_base == MAP_FAILED) {
            const auto map_errno = errno;
            if (active_device_.logger != nullptr) {
                std::string message = "allocate drv=none host memory failed: " +
                                      std::string(std::strerror(map_errno));
                if (hugepage_errno != 0) {
                    message += "; 2 MiB HugeTLB failed: ";
                    message += std::strerror(hugepage_errno);
                }
                active_device_.logger->error(message);
            }
            return {};
        }

        auto* bytes = static_cast<volatile uint8_t*>(cpu_base);
        for (uint64_t offset = 0; offset < mapped_size; offset += page_size) {
            bytes[offset] = 0;
        }
        if (mlock(cpu_base, static_cast<size_t>(mapped_size)) != 0) {
            const auto lock_errno = errno;
            munmap(cpu_base, static_cast<size_t>(mapped_size));
            if (active_device_.logger != nullptr) {
                active_device_.logger->error(
                    "mlock drv=none host memory failed: " +
                    std::string(std::strerror(lock_errno)));
            }
            return {};
        }

        uint64_t physical_address = 0;
        if (!resolve_contiguous_physical(cpu_base, mapped_size, page_size,
                                         physical_address, error) ||
            physical_address % DEFAULT_MEMORY_ALIGNMENT != 0) {
            munlock(cpu_base, static_cast<size_t>(mapped_size));
            munmap(cpu_base, static_cast<size_t>(mapped_size));
            if (active_device_.logger != nullptr) {
                if (error.empty()) {
                    error = "host DMA physical address is not 512-byte aligned";
                }
                if (hugepage_errno != 0 && !used_hugepage) {
                    error += "; 2 MiB HugeTLB allocation also failed: ";
                    error += std::strerror(hugepage_errno);
                    if (!hugepage_reservation_error.empty()) {
                        error += "; automatic reservation failed: ";
                        error += hugepage_reservation_error;
                    }
                }
                active_device_.logger->error(error);
            }
            return {};
        }

        if (active_device_.logger != nullptr) {
            std::ostringstream address;
            address << std::hex << physical_address;
            active_device_.logger->flow(
                "drv=none host DMA buffer: requested_bytes=" +
                std::to_string(size_bytes) +
                " mapped_bytes=" + std::to_string(mapped_size) +
                " allocator=" + (used_hugepage ? "hugetlb-2m" : "anonymous") +
                " physical_address=0x" + address.str());
        }
        return DmaBuffer(this,
                         cpu_base,
                         size_bytes,
                         physical_address,
                         0,
                         -1,
                         mapped_size,
                         true,
                         "physical");
    }

    int fd = open_device(active_device_.bdf);
    if (fd < 0) {
        if (active_device_.logger != nullptr) {
            active_device_.logger->error(
                "drv=isml_drv requires a matching /dev/isml_drv device for " +
                active_device_.bdf);
        }
        return {};
    }

    isml_diag_dma_alloc request = {};
    request.size = size_bytes;
    if (ioctl(fd, ISML_DIAG_IOCTL_DMA_ALLOC, &request) != 0) {
        const auto ioctl_errno = errno;
        close(fd);
        if (active_device_.logger != nullptr) {
            active_device_.logger->error(
                "isml_drv DMA allocation failed: " +
                std::string(std::strerror(ioctl_errno)));
        }
        return {};
    }

    if (request.device_addr % DEFAULT_MEMORY_ALIGNMENT != 0) {
        isml_diag_dma_free release = {request.handle};
        ioctl(fd, ISML_DIAG_IOCTL_DMA_FREE, &release);
        close(fd);
        if (active_device_.logger != nullptr) {
            active_device_.logger->error(
                "isml_drv DMA allocation is not 512-byte aligned");
        }
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
                     request.size,
                     false,
                     "dma_addr");
}

void HalContext::free_host_dma_buffer(DmaBuffer& buffer)
{
    if (buffer.backend_fd_ >= 0) {
        if (buffer.cpu_base_ != nullptr && buffer.mapped_size_ != 0) {
            munmap(buffer.cpu_base_, static_cast<size_t>(buffer.mapped_size_));
        }
        isml_diag_dma_free request = {buffer.handle_};
        ioctl(buffer.backend_fd_, ISML_DIAG_IOCTL_DMA_FREE, &request);
        close(buffer.backend_fd_);
    } else if (buffer.cpu_base_ != nullptr && buffer.mapped_size_ != 0) {
        if (buffer.memory_locked_) {
            munlock(buffer.cpu_base_, static_cast<size_t>(buffer.mapped_size_));
        }
        munmap(buffer.cpu_base_, static_cast<size_t>(buffer.mapped_size_));
    }
    buffer.alloc_ctx_ = nullptr;
    buffer.cpu_base_ = nullptr;
    buffer.device_addr_ = 0;
    buffer.size_ = 0;
    buffer.handle_ = 0;
    buffer.backend_fd_ = -1;
    buffer.mapped_size_ = 0;
    buffer.memory_locked_ = false;
    buffer.addr_kind_.clear();
}

void HalContext::clear_mappings()
{
    phal_context_ = {};
    aperture_phal_context_ = {};
    for (const auto& mapping : mapped_bar_mappings_) {
        if (mapping.first != nullptr && mapping.second != 0) {
            munmap(mapping.first, static_cast<size_t>(mapping.second));
        }
    }
    mapped_bar_mappings_.clear();
}

void HalContext::clear()
{
    clear_mappings();
    active_device_ = {};
    active_memory_regions_ = {};
    active_memory_ = std::make_shared<DeviceMemoryState>();
    host_dma_mode_ = HostDmaMode::IsmlDrv;
    device_bound_ = false;
}
