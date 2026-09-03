#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PhalBridge.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class HalType {
    pHal,
    iHal,
};

std::string to_string(HalType type);

class HalContext;

class DmaBuffer {
private:
    friend class HalContext;

    HalContext* context_ = nullptr;
    std::vector<uint8_t> storage_;
    uint64_t device_addr_ = 0;
    uint64_t size_ = 0;
    uint64_t backend_handle_ = 0;
    std::string addr_kind_;

    DmaBuffer(HalContext* context,
              std::vector<uint8_t> storage,
              uint64_t device_addr,
              uint64_t backend_handle,
              std::string addr_kind);

public:
    DmaBuffer() = default;
    ~DmaBuffer();

    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;

    DmaBuffer(DmaBuffer&& other) noexcept;
    DmaBuffer& operator=(DmaBuffer&& other) noexcept;

    void release();

    bool valid() const { return context_ != nullptr && !storage_.empty(); }
    void* cpu_base() { return storage_.empty() ? nullptr : storage_.data(); }
    const void* cpu_base() const { return storage_.empty() ? nullptr : storage_.data(); }
    uint64_t device_addr() const { return device_addr_; }
    uint64_t size() const { return size_; }
    uint64_t backend_handle() const { return backend_handle_; }
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

    void reset(HalType type);
    std::vector<DeviceContext> scan_pci_devices() const;
    DeviceContext mmap_bar_space(DeviceContext ctx);
    DevMem open_devmem(const DeviceContext& ctx, DevMemSpec spec, Logger* logger = nullptr);
    std::vector<DevMem> open_multi_devmem(const DeviceContext& ctx,
                                          std::vector<DevMemSpec> specs,
                                          Logger* logger = nullptr);
    DmaBuffer alloc_host_buffer(const DeviceContext& ctx, uint64_t size_bytes);
    void free_host_buffer(DmaBuffer& buffer);
    void clear();
};
