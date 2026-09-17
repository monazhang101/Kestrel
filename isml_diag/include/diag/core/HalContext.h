#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PhalBridge.h"

#include <cstdint>
#include <memory>
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
    std::vector<std::unique_ptr<std::vector<uint8_t>>> mapped_bar_storage_;
    std::vector<std::pair<void*, uint64_t>> mapped_bar_mappings_;

    void clear_mappings();

public:
    explicit HalContext(HalType type = HalType::iHal);

    HalType type() const { return type_; }
    PhalBridge& phal() { return phal_bridge_; }
    const PhalBridge& phal() const { return phal_bridge_; }

    void reset(HalType type);
    std::vector<DeviceContext> scan_pci_devices() const;
    DeviceContext mmap_bar_space(DeviceContext ctx);
    DmaBuffer alloc_host_dma_buffer(const DeviceContext& ctx, uint64_t size_bytes);
    void free_host_dma_buffer(DmaBuffer& buffer);
    void clear();
};
