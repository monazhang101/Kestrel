#include "diag/core/HalBackend.h"

#include <atomic>
#include <limits>

namespace {

uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

uint64_t reserve_fake_dma_addr(uint64_t size_bytes)
{
    static std::atomic<uint64_t> next_addr{0x10000000};
    return next_addr.fetch_add(align_up(size_bytes, 0x1000));
}

uint64_t next_fake_backend_handle()
{
    static std::atomic<uint64_t> next_handle{1};
    return next_handle.fetch_add(1);
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
    session_->free_dma_buffer(*this);
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
    // Pseudocode:
    // pHal can walk /sys/bus/pci/devices directly.
    // iHal can delegate discovery to the integrated/vendor HAL service.
    DeviceContext tpu0;
    tpu0.bdf = "0000:65:00.0";
    tpu0.vendor_id = 0x1d0f;
    tpu0.device_id = 0x1000;

    DeviceContext tpu1;
    tpu1.bdf = "0000:ca:00.0";
    tpu1.vendor_id = 0x1d0f;
    tpu1.device_id = 0x1000;

    DeviceContext unrelated_device;
    unrelated_device.bdf = "0000:17:00.0";
    unrelated_device.vendor_id = 0xffff;
    unrelated_device.device_id = 0xffff;

    return {tpu0, tpu1, unrelated_device};
}

DeviceContext HalBackend::mmap_bar_space(DeviceContext ctx)
{
    // Pseudocode:
    // pHal can open /sys/bus/pci/devices/<bdf>/resource0 or a VFIO region.
    // iHal can request an equivalent BAR/MMIO mapping from its backend service.
    mapped_bar_storage_.push_back(
        std::make_unique<std::vector<uint8_t>>(DEFAULT_BAR_SIZE, 0));

    ctx.mapped_bar_base = mapped_bar_storage_.back()->data();
    ctx.bar_size = DEFAULT_BAR_SIZE;
    return ctx;
}

DmaBuffer HalBackend::alloc_dma_buffer(HalSession* session,
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

void HalBackend::free_dma_buffer(DmaBuffer& buffer)
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

DmaBuffer HalSession::alloc_dma_buffer(const DeviceContext& ctx, uint64_t size_bytes)
{
    return backend_.alloc_dma_buffer(this, ctx, size_bytes);
}

void HalSession::free_dma_buffer(DmaBuffer& buffer)
{
    backend_.free_dma_buffer(buffer);
}

void HalSession::clear()
{
    backend_.clear_mappings();
}
