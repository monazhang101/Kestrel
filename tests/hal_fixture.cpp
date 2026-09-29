#include "diag/core/HalContext.h"

extern "C" {
#include <atlas_csr_sw/src/h/TOP.h>
}

// Test-only replacement for the Linux mapping owner. Real ioctls and DMA are
// deliberately unavailable. The actual PHAL bridge and IO code are compiled.
HalContext::HalContext(HalType type) : type_(type) {}
void HalContext::clear_mappings()
{
    phal_context_ = {};
    aperture_phal_context_ = {};
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
std::string to_string(HalType type) { return type == HalType::iHal ? "iHal" : "pHal"; }

void HalContext::bind_device(const DeviceContext& ctx, Logger* logger)
{
    phal_context_ = {};
    aperture_phal_context_ = {};
    active_device_ = ctx;
    active_device_.logger = logger != nullptr ? logger : ctx.logger;
    active_memory_regions_ = ctx.memory_regions;
    active_memory_ = ctx.memory != nullptr
        ? ctx.memory
        : std::make_shared<DeviceMemoryState>();
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
    aperture_phal_context_ = phal_bridge_.create_context(active_device_, project, &init_error);
    if (!aperture_phal_context_) {
        phal_context_ = {};
        if (error != nullptr) *error = "aperture PHAL initialization failed: " + init_error;
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
    if (!operation || !device_bound_ || !aperture_phal_context_) {
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
    phal->env.base = PhalBridge::CHIP_RCF_BASE;
    return status;
}

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
        bar.mapped_size = 0x10000;
        bar.mapped = bar.writable = true;
        ctx.bar_mappings.push_back(bar);
    }
    return ctx;
}

DmaBuffer HalContext::host_mem_alloc(uint64_t) { return {}; }
void HalContext::free_host_dma_buffer(DmaBuffer& buffer) { buffer.alloc_ctx_ = nullptr; }
DmaBuffer::~DmaBuffer() { release(); }
void DmaBuffer::release() { if (alloc_ctx_) alloc_ctx_->free_host_dma_buffer(*this); }
