#include "diag/core/HalBackend.h"

#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <sstream>

#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#endif

namespace {

constexpr std::array<uint32_t, 3> kProbeBarIndices = {0, 2, 4};

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

uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

uint64_t reserve_fake_dma_addr(uint64_t size_bytes)
{
    // Temporary dry-run address allocator; real backends should return HAL-owned addresses.
    static std::atomic<uint64_t> next_addr{0x10000000};
    return next_addr.fetch_add(align_up(size_bytes, 0x1000));
}

uint64_t next_fake_backend_handle()
{
    // Temporary dry-run handle generator; real backends should return backend resource handles.
    static std::atomic<uint64_t> next_handle{1};
    return next_handle.fetch_add(1);
}

#ifdef __linux__
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
                        uint64_t& size)
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

    size = end - start + 1;
    return size != 0;
}
#endif

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

DmaBuffer::DmaBuffer(HalSession* session,
                     std::vector<uint8_t> storage,
                     uint64_t device_addr,
                     uint64_t backend_handle,
                     std::string addr_kind)
    : session_(session),
      storage_(std::move(storage)),
      device_addr_(device_addr),
      size_(static_cast<uint64_t>(storage_.size())),
      backend_handle_(backend_handle),
      addr_kind_(std::move(addr_kind))
{
}

DmaBuffer::~DmaBuffer()
{
    release();
}

DmaBuffer::DmaBuffer(DmaBuffer&& other) noexcept
    : session_(other.session_),
      storage_(std::move(other.storage_)),
      device_addr_(other.device_addr_),
      size_(other.size_),
      backend_handle_(other.backend_handle_),
      addr_kind_(std::move(other.addr_kind_))
{
    other.session_ = nullptr;
    other.device_addr_ = 0;
    other.size_ = 0;
    other.backend_handle_ = 0;
}

DmaBuffer& DmaBuffer::operator=(DmaBuffer&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    release();

    session_ = other.session_;
    storage_ = std::move(other.storage_);
    device_addr_ = other.device_addr_;
    size_ = other.size_;
    backend_handle_ = other.backend_handle_;
    addr_kind_ = std::move(other.addr_kind_);

    other.session_ = nullptr;
    other.device_addr_ = 0;
    other.size_ = 0;
    other.backend_handle_ = 0;

    return *this;
}

void DmaBuffer::release()
{
    if (session_ == nullptr) {
        return;
    }
    session_->free_host_buffer(*this);
}

HalBackend::HalBackend(HalType type)
    : type_(type)
{
}

void HalBackend::reset(HalType type)
{
    clear_mappings();
    phal_bridge_.reset();
    type_ = type;
}

std::vector<DeviceContext> HalBackend::scan_pci_devices() const
{
    if (type_ == HalType::iHal) {
#ifdef __linux__
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
#else
        return {};
#endif
    }

    // Pseudocode:
    // pHal can delegate discovery to a platform/vendor API.
    // Until that API is wired in, pHal keeps a fake device inventory for dry-run.
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

DeviceContext HalBackend::mmap_bar_space(DeviceContext ctx)
{
    ctx.bar_mappings.clear();
    ctx.mapped_bar_base = nullptr;
    ctx.bar_device_base = 0;
    ctx.bar_size = 0;

    if (type_ == HalType::iHal) {
#ifdef __linux__
        for (auto bar_index : kProbeBarIndices) {
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
            bar.size = map_size;
            if (mapped == MAP_FAILED) {
                bar.error = "mmap resource" + std::to_string(bar_index) +
                            " failed: " + std::strerror(mmap_errno);
                ctx.bar_mappings.push_back(std::move(bar));
                continue;
            }

            bar.mapped_base = mapped;
            bar.mapped = true;
            mapped_bar_mappings_.push_back({mapped, map_size});

            if (bar_index == 0) {
                ctx.mapped_bar_base = mapped;
                ctx.bar_device_base = bar_start;
                ctx.bar_size = map_size;
            }

            ctx.bar_mappings.push_back(std::move(bar));
        }
        return ctx;
#else
        for (auto bar_index : kProbeBarIndices) {
            auto bar = make_bar_mapping(bar_index);
            bar.error = "BAR mmap is only available on Linux for iHal";
            ctx.bar_mappings.push_back(std::move(bar));
        }
        return ctx;
#endif
    }

    // Pseudocode:
    // pHal can request equivalent BAR metadata from its backend service:
    //   host-mapped pointer -> mapped_bar_base
    //   device-visible BAR base, e.g. dev->bars[N].baseAddr -> bar_device_base
    //   BAR length -> bar_size
    // Until that API is wired in, pHal keeps fake BAR mappings for dry-run.
    for (auto bar_index : kProbeBarIndices) {
        mapped_bar_storage_.push_back(
            std::make_unique<std::vector<uint8_t>>(DRYRUN_BAR_WINDOW_SIZE, 0));

        auto bar = make_bar_mapping(bar_index);
        bar.mapped_base = mapped_bar_storage_.back()->data();
        bar.device_base = fake_bar_device_base_for_bdf(ctx.bdf, bar_index);
        bar.resource_size = bar.expected_size;
        bar.mapped_size = DRYRUN_BAR_WINDOW_SIZE;
        bar.size = DRYRUN_BAR_WINDOW_SIZE;
        bar.mapped = true;

        if (bar_index == 0) {
            ctx.mapped_bar_base = bar.mapped_base;
            ctx.bar_device_base = bar.device_base;
            ctx.bar_size = bar.size;
        }

        ctx.bar_mappings.push_back(std::move(bar));
    }
    return ctx;
}

DmaBuffer HalBackend::alloc_host_buffer(HalSession* session,
                                        const DeviceContext& ctx,
                                        uint64_t size_bytes)
{
    (void)ctx;

    if (session == nullptr ||
        size_bytes == 0 ||
        size_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return {};
    }

    // Pseudocode:
    // iHal will allocate through its kernel module and mmap the CPU view.
    // pHal will call its own abstract DMA-buffer allocation API.
    std::vector<uint8_t> storage(static_cast<size_t>(size_bytes), 0);
    return DmaBuffer(session,
                     std::move(storage),
                     reserve_fake_dma_addr(size_bytes),
                     next_fake_backend_handle(),
                     type_ == HalType::iHal ? "iova" : "phal_handle");
}

DevMem HalBackend::open_dev_mem(const DeviceContext& ctx, DevMemSpec spec, Logger* logger)
{
    if (spec.size == 0) {
        return DevMem::invalid("dev_mem size must be nonzero");
    }

    if (spec.project == PhalProject::Generic) {
        spec.project = phal_project_from_tpu_type(ctx.tpu_type);
    }

    IhalIO ihalIO;
    ihalIO.device_ctx = &ctx;
    ihalIO.bar_index = spec.control_bar_index;
    ihalIO.module_base = spec.control_module_base;

    std::string error;
    if (!phal_bridge_.init(ihalIO, spec.project, &error)) {
        return DevMem::invalid("phal init failed: " + error);
    }

    return DevMem(ctx, spec, &phal_bridge_, logger);
}

void HalBackend::free_host_buffer(DmaBuffer& buffer)
{
    buffer.storage_.clear();
    buffer.session_ = nullptr;
    buffer.device_addr_ = 0;
    buffer.size_ = 0;
    buffer.backend_handle_ = 0;
    buffer.addr_kind_.clear();
}

void HalBackend::clear_mappings()
{
#ifdef __linux__
    if (type_ == HalType::iHal) {
        for (const auto& mapping : mapped_bar_mappings_) {
            if (mapping.first != nullptr && mapping.second != 0) {
                munmap(mapping.first, static_cast<size_t>(mapping.second));
            }
        }
        mapped_bar_mappings_.clear();
    }
#endif
    mapped_bar_storage_.clear();
    phal_bridge_.reset();
}

HalSession::HalSession(HalType type)
    : backend_(type)
{
}

void HalSession::reset(HalType type)
{
    backend_.reset(type);
}

std::vector<DeviceContext> HalSession::scan_pci_devices() const
{
    return backend_.scan_pci_devices();
}

DeviceContext HalSession::mmap_bar_space(DeviceContext ctx)
{
    return backend_.mmap_bar_space(ctx);
}

DmaBuffer HalSession::alloc_host_buffer(const DeviceContext& ctx, uint64_t size_bytes)
{
    return backend_.alloc_host_buffer(this, ctx, size_bytes);
}

void HalSession::free_host_buffer(DmaBuffer& buffer)
{
    backend_.free_host_buffer(buffer);
}

DevMem HalSession::open_dev_mem(const DeviceContext& ctx, DevMemSpec spec, Logger* logger)
{
    return backend_.open_dev_mem(ctx, spec, logger);
}

void HalSession::clear()
{
    backend_.clear_mappings();
}
