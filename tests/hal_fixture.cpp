#include "diag/core/HalContext.h"

// Test-only replacement for the Linux mapping owner. Real ioctls and DMA are
// deliberately unavailable. The actual PHAL bridge and IO code are compiled.
HalContext::HalContext(HalType type) : type_(type) {}
void HalContext::reset(HalType type) { clear(); type_ = type; }
void HalContext::clear_mappings() { phal_bridge_.reset(); mapped_bar_storage_.clear(); }
void HalContext::clear() { clear_mappings(); }
std::string to_string(HalType type) { return type == HalType::iHal ? "iHal" : "pHal"; }

std::vector<DeviceContext> HalContext::scan_pci_devices() const
{
    DeviceContext device;
    device.bdf = "0000:19:00.0";
    device.vendor_id = 0x16c3;
    device.device_id = 0xabcd;
    return {device};
}

DeviceContext HalContext::mmap_bar_space(DeviceContext ctx)
{
    ctx.bar_mappings.clear();
    for (uint32_t index : {0u, 2u, 4u}) {
        mapped_bar_storage_.push_back(std::make_unique<std::vector<uint8_t>>(0x10000));
        BarMapping bar;
        bar.bar_index = index;
        bar.name = "test_bar" + std::to_string(index);
        bar.mapped_base = mapped_bar_storage_.back()->data();
        bar.mapped_size = bar.size = 0x10000;
        bar.mapped = bar.writable = true;
        ctx.bar_mappings.push_back(bar);
    }
    return ctx;
}

DmaBuffer HalContext::alloc_host_dma_buffer(const DeviceContext&, uint64_t) { return {}; }
void HalContext::free_host_dma_buffer(DmaBuffer& buffer) { buffer.alloc_ctx_ = nullptr; }
DmaBuffer::~DmaBuffer() { release(); }
void DmaBuffer::release() { if (alloc_ctx_) alloc_ctx_->free_host_dma_buffer(*this); }
