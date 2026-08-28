#include "diag/core/HalBackend.h"

#include <algorithm>
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

uint64_t fake_bar_device_base_for_bdf(const std::string& bdf)
{
    // Dry-run BAR device-visible base. Real backends should discover this from:
    //   pHal: /sys/bus/pci/devices/<bdf>/resourceN start address or VFIO region info.
    //   iHal: vendor HAL device BAR metadata, for example dev->bars[N].baseAddr.
    // This is the address a device DMA descriptor should use, not the CPU
    // virtual pointer returned by mmap/VFIO mmap.
    if (bdf == "0000:19:00.0" || bdf == "0000:65:00.0") {
        return 0x80000000;
    }
    if (bdf == "0000:ca:00.0") {
        return 0x90000000;
    }
    return 0;
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

bool read_resource0_info(const std::string& bdf,
                         uint64_t& start,
                         uint64_t& size)
{
    std::ifstream input("/sys/bus/pci/devices/" + bdf + "/resource");
    std::string line;
    if (!std::getline(input, line)) {
        return false;
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
    if (type_ == HalType::iHal) {
#ifdef __linux__
        uint64_t bar_start = 0;
        uint64_t bar_size = 0;
        if (!read_resource0_info(ctx.bdf, bar_start, bar_size)) {
            ctx.mapped_bar_base = nullptr;
            ctx.bar_device_base = 0;
            ctx.bar_size = 0;
            return ctx;
        }

        auto resource0 = "/sys/bus/pci/devices/" + ctx.bdf + "/resource0";
        int fd = open(resource0.c_str(), O_RDWR | O_SYNC);
        int prot = PROT_READ | PROT_WRITE;
        if (fd < 0) {
            fd = open(resource0.c_str(), O_RDONLY | O_SYNC);
            prot = PROT_READ;
        }
        if (fd < 0) {
            ctx.mapped_bar_base = nullptr;
            ctx.bar_device_base = bar_start;
            ctx.bar_size = bar_size;
            return ctx;
        }

        const uint64_t map_size = std::min(bar_size, DEFAULT_BAR_SIZE);
        void* mapped = mmap(nullptr,
                            static_cast<size_t>(map_size),
                            prot,
                            MAP_SHARED,
                            fd,
                            0);
        close(fd);

        if (mapped == MAP_FAILED) {
            ctx.mapped_bar_base = nullptr;
            ctx.bar_device_base = bar_start;
            ctx.bar_size = bar_size;
            return ctx;
        }

        ctx.mapped_bar_base = mapped;
        ctx.bar_device_base = bar_start;
        ctx.bar_size = map_size;
        mapped_bar_mappings_.push_back({mapped, map_size});
        return ctx;
#else
        ctx.mapped_bar_base = nullptr;
        ctx.bar_device_base = 0;
        ctx.bar_size = 0;
        return ctx;
#endif
    }

    // Pseudocode:
    // pHal can request equivalent BAR metadata from its backend service:
    //   host-mapped pointer -> mapped_bar_base
    //   device-visible BAR base, e.g. dev->bars[N].baseAddr -> bar_device_base
    //   BAR length -> bar_size
    // Until that API is wired in, pHal keeps a fake BAR mapping for dry-run.
    mapped_bar_storage_.push_back(
        std::make_unique<std::vector<uint8_t>>(DEFAULT_BAR_SIZE, 0));

    ctx.mapped_bar_base = mapped_bar_storage_.back()->data();
    ctx.bar_device_base = fake_bar_device_base_for_bdf(ctx.bdf);
    ctx.bar_size = DEFAULT_BAR_SIZE;
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

void HalSession::clear()
{
    backend_.clear_mappings();
}
