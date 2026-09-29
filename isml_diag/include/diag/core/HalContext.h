#pragma once

#include "diag/core/Platform.h"
#include "diag/core/PhalBridge.h"
#include "diag/core/DevMem.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Public C++ view of the per-allocation host DMA limit. HalContext.cpp checks
// this against the authoritative Linux UAPI constant at compile time.
inline constexpr uint64_t HOST_DMA_MAX_SIZE_BYTES = 128ull * 1024ull * 1024ull;

enum class HalType {
    pHal,
    iHal,
};

std::string to_string(HalType type);

class HalContext;

class DmaBuffer {
private:
    friend class HalContext;

    HalContext* alloc_ctx_ = nullptr;
    void* cpu_base_ = nullptr;
    uint64_t device_addr_ = 0;
    uint64_t size_ = 0;
    uint64_t handle_ = 0;
    int backend_fd_ = -1;
    std::string addr_kind_;

    DmaBuffer(HalContext* alloc_ctx,
              void* cpu_base,
              uint64_t size_bytes,
              uint64_t device_addr,
              uint64_t handle,
              int backend_fd,
              std::string addr_kind);

public:
    DmaBuffer() = default;
    ~DmaBuffer();

    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;

    DmaBuffer(DmaBuffer&& other) noexcept;
    DmaBuffer& operator=(DmaBuffer&& other) noexcept;

    void release();

    bool valid() const { return alloc_ctx_ != nullptr && cpu_base_ != nullptr && size_ != 0; }
    void* cpu_base() { return cpu_base_; }
    const void* cpu_base() const { return cpu_base_; }
    uint64_t device_addr() const { return device_addr_; }
    uint64_t size() const { return size_; }
    uint64_t handle() const { return handle_; }
    const std::string& addr_kind() const { return addr_kind_; }
};

class HalContext {
private:
    static constexpr uint64_t DRYRUN_BAR_WINDOW_SIZE = 0x10000;

    HalType type_ = HalType::iHal;
    PhalBridge phal_bridge_;
    // These contexts exist only while one testcase is running. The aperture
    // context is separate because PCIe block entry changes env.base.
    PhalBridge::ScopedContext phal_context_;
    PhalBridge::ScopedContext aperture_phal_context_;
    DeviceContext active_device_;
    MemoryRegionMap active_memory_regions_{};
    std::shared_ptr<DeviceMemoryState> active_memory_ =
        std::make_shared<DeviceMemoryState>();
    bool device_bound_ = false;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> mapped_bar_storage_;
    std::vector<std::pair<void*, uint64_t>> mapped_bar_mappings_;

    void clear_mappings();

public:
    class TestcaseScope {
    private:
        HalContext* owner_ = nullptr;
        explicit TestcaseScope(HalContext* owner) : owner_(owner) {}
        friend class HalContext;

    public:
        TestcaseScope() = default;
        ~TestcaseScope();
        TestcaseScope(const TestcaseScope&) = delete;
        TestcaseScope& operator=(const TestcaseScope&) = delete;
        TestcaseScope(TestcaseScope&& other) noexcept;
        TestcaseScope& operator=(TestcaseScope&& other) noexcept;
        explicit operator bool() const { return owner_ != nullptr; }
    };

    explicit HalContext(HalType type = HalType::iHal);

    HalType type() const { return type_; }

    std::vector<DeviceContext> scan_pci_devices() const;
    DeviceContext mmap_bar_space(DeviceContext ctx);
    void bind_device(const DeviceContext& ctx, Logger* logger = nullptr);
    // Create the two PHAL contexts used during one testcase. The returned
    // scope owns their lifetime and deinitializes them on testcase exit.
    TestcaseScope begin_testcase(std::string* error = nullptr);
    const DeviceContext& device_context() const;
    bool has_device() const { return device_bound_; }
    const MemoryRegionMap& memory_regions() const { return active_memory_regions_; }

    // The ordinary testcase context is exposed only to framework-created
    // TestInfo instances. Device-memory callers use with_aperture_context().
    phal_ctx_t* phal_context() const { return phal_context_.get(); }

    // Run one complete aperture operation while holding the per-device
    // aperture lock. The callback receives a context rooted at CHIP_RCF_BASE
    // and entered into the PCIe block exactly once.
    TestStatus with_aperture_context(
        const std::function<TestStatus(phal_ctx_t*)>& operation,
        std::string* error = nullptr);

    common::MemBuffer device_mem_alloc(
        MemoryRegion region,
        uint64_t size_bytes,
        std::optional<uint64_t> fixed_offset = std::nullopt);
    DmaBuffer host_mem_alloc(uint64_t size_bytes);
    void free_host_dma_buffer(DmaBuffer& buffer);
    void clear();
};
