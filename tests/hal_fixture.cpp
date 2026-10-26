#include "diag/core/HalContext.h"

#include <unordered_map>
#include <algorithm>
#include <new>

extern "C" {
#include <atlas_csr_sw/src/h/TOP.h>
}

namespace {

using TestBarStorage = std::vector<std::unique_ptr<std::vector<uint8_t>>>;
std::unordered_map<HalContext*, TestBarStorage> test_bar_storage;

}

// Test-only replacement for the Linux mapping owner. Real ioctls and DMA are
// deliberately unavailable. The actual PHAL bridge and IO code are compiled.
HalContext::HalContext() = default;
void HalContext::clear_mappings()
{
    phal_context_ = {};
    aperture_phal_context_ = {};
    test_bar_storage.erase(this);
}
void HalContext::clear()
{
    clear_mappings();
    active_device_ = {};
    active_memory_regions_ = {};
    active_memory_ = std::make_shared<DeviceMemoryState>();
    device_bound_ = false;
}
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

TestStatus HalContext::with_pcie_context(
    const std::function<TestStatus(phal_ctx_t*)>& operation,
    std::string* error)
{
    if (error != nullptr) error->clear();
    if (!operation || !device_bound_ || !aperture_phal_context_) {
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
    phal->env.base = PhalBridge::CHIP_RCF_BASE;
    return status;
}

TestStatus HalContext::secondary_bus_reset(bool)
{
    return PHAL_STATUS_UNIMPLEMENTED;
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
    auto& storage = test_bar_storage[this];
    for (uint32_t index : {0u, 2u, 4u}) {
        const auto mapped_size = static_cast<size_t>(0x10000);
        storage.push_back(
            std::make_unique<std::vector<uint8_t>>(mapped_size));
        BarMapping bar;
        bar.bar_index = index;
        bar.name = "test_bar" + std::to_string(index);
        bar.mapped_base = storage.back()->data();
        bar.mapped_size = mapped_size;
        bar.mapped = bar.writable = true;
        ctx.bar_mappings.push_back(bar);
    }
    return ctx;
}

// Explicit opt-in for the CP register-protocol tests. Normal CLI fixtures
// still reject host allocation. These addresses are NOT hardware DMA buffers.
bool test_host_allocations_enabled = false;
static std::vector<void*> test_host_storage;
void release_test_host_storage()
{
    for (auto* ptr : test_host_storage)
        ::operator delete(ptr, std::align_val_t(DEFAULT_MEMORY_ALIGNMENT));
    test_host_storage.clear();
}
DmaBuffer::DmaBuffer(HalContext* owner, void* cpu, uint64_t size,
                     uint64_t address, uint64_t handle, int fd,
                     uint64_t mapped_size, bool memory_locked, std::string kind)
    : alloc_ctx_(owner), cpu_base_(cpu), device_addr_(address), size_(size),
      handle_(handle), backend_fd_(fd), mapped_size_(mapped_size),
      memory_locked_(memory_locked), addr_kind_(std::move(kind)) {}
DmaBuffer HalContext::host_mem_alloc(uint64_t bytes)
{
    if (!test_host_allocations_enabled || !bytes || bytes > HOST_DMA_MAX_SIZE_BYTES) return {};
    auto* data = ::operator new(bytes, std::align_val_t(DEFAULT_MEMORY_ALIGNMENT));
    test_host_storage.push_back(data);
    return DmaBuffer(this, data, bytes, 0x120000000ull, 1, -1,
                     0, false, "test-only");
}
void HalContext::free_host_dma_buffer(DmaBuffer& buffer)
{
    const auto it = std::find(test_host_storage.begin(), test_host_storage.end(), buffer.cpu_base_);
    if (it != test_host_storage.end()) {
        ::operator delete(*it, std::align_val_t(DEFAULT_MEMORY_ALIGNMENT));
        test_host_storage.erase(it);
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
DmaBuffer::~DmaBuffer() { release(); }
void DmaBuffer::release() { if (alloc_ctx_) alloc_ctx_->free_host_dma_buffer(*this); }
