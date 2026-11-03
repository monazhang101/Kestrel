#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <type_traits>

struct DeviceContext;

/*
 * IPC opcodes understood by the firmware side (mirror of op_code_t in
 * ipc_rw.h; kept separate so the diag tree does not include firmware
 * headers directly).
 */
enum class IpcOp : uint32_t {
    Read = 0,
    Write = 1,
};

enum class IpcStatus : uint32_t {
    Ok = 0,
    ErrInvalid = 1u << 0,
    ErrTimeout = 1u << 1,
    ErrUnknown = 1u << 2,
};

// RCF region offset inside the host BAR0 window.
constexpr uint32_t IPC_RCF_BAR0_OFFSET = 0x20000000u;

// Host-side mirror of TARGET_CORE_* in abios/common/ipc_rw/ipc_rw.h.
enum class IpcFwId : uint32_t {
    Pmu = 2,
    Hqc = 3,
    Ddp0 = 4,
    Ddp1 = 5,
    Ddp2 = 6,
    Ddp3 = 7,
};

constexpr uint32_t IPC_FW_ID_FIRST = 2;
constexpr uint32_t IPC_FW_ID_LAST = 7;
constexpr size_t IPC_FW_COUNT = IPC_FW_ID_LAST - IPC_FW_ID_FIRST + 1;

const char* to_string(IpcFwId fw);

constexpr uint32_t IPC_CORE_STRIDE = 0x1Cu;

struct IpcRegionConfig {
    uint32_t base;
    uint32_t size;
    uint32_t core_id;
    bool provisioned;
};

// base is the firmware's RCF-view address. bind() adds the BAR0 RCF offset.
// Placeholder rows must remain unprovisioned until firmware assigns a region.
constexpr IpcRegionConfig IPC_REGION_CONFIGS[IPC_FW_COUNT] = {
    /* Pmu  */ {0x1FF800u, 0x1000u, 1u, false},
    /* Hqc  */ {0x16D1F780u, 0x1000u, 1u, true},
    /* Ddp0 */ {0x12C80720u, 0xE0u, 1u, true},
    /* Ddp1 */ {0x13C80720u, 0xE0u, 1u, true},
    /* Ddp2 */ {0x14C80720u, 0xE0u, 1u, true},
    /* Ddp3 */ {0x15C80720u, 0xE0u, 1u, true},
};

constexpr IpcRegionConfig ipc_region_config(IpcFwId fw)
{
    using U = std::underlying_type_t<IpcFwId>;
    const auto id = static_cast<U>(fw);
    const auto i = id < IPC_FW_ID_FIRST || id > IPC_FW_ID_LAST
        ? static_cast<U>(IpcFwId::Hqc) - IPC_FW_ID_FIRST
        : id - IPC_FW_ID_FIRST;
    return IPC_REGION_CONFIGS[i];
}

class IpcBridge {
public:
    IpcBridge() = default;
    ~IpcBridge() = default;
    IpcBridge(const IpcBridge&) = delete;
    IpcBridge& operator=(const IpcBridge&) = delete;

    bool bind(const DeviceContext& ctx, IpcFwId fw,
              std::string* error = nullptr);
    bool check_magic(const DeviceContext& ctx,
                     std::string* error = nullptr) const;

    bool ready() const { return window_ != nullptr; }

    IpcStatus read(uint32_t addr, uint32_t& value,
                   uint32_t poll_max = default_poll_max());
    IpcStatus write(uint32_t addr, uint32_t value,
                    uint32_t poll_max = default_poll_max());

    uint64_t region_bar0_base() const { return bar0_offset_; }
    uint64_t slot_region_offset() const
    {
        return static_cast<uint64_t>(IPC_CORE_STRIDE) * core_id_;
    }
    uint64_t slot_bar0_offset() const
    {
        return bar0_offset_ + slot_region_offset();
    }

    static constexpr uint32_t default_poll_max() { return 1000000u; }
    static constexpr uint32_t default_timeout_ms() { return 1000u; }

private:
    bool bind(const DeviceContext& ctx, IpcFwId fw,
              const IpcRegionConfig& cfg, std::string* error);
    volatile uint32_t* slot_() const;
    IpcStatus transact(IpcOp op, uint32_t addr, uint32_t data,
                       uint32_t& value_out, uint32_t poll_max,
                       uint32_t timeout_ms);

    void* window_ = nullptr;
    uint64_t window_size_ = 0;
    uint64_t bar0_offset_ = 0;
    uint64_t region_base_ = 0;
    uint64_t region_size_ = 0;
    uint32_t core_id_ = 0;
    std::mutex lock_;
};

IpcStatus to_ipc_status_text(IpcStatus status, IpcFwId fw,
                             std::string& text);
