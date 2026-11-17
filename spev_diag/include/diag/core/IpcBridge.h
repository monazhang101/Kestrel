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

// Status codes mirror the firmware's IPC_* codes (value field on error).
enum class IpcStatus : uint32_t {
    Ok = 0,
    ErrInvalid = 1u << 0,
    ErrTimeout = 1u << 1,
    ErrUnknown = 1u << 2,
};

/*
 * Offset of the RCF region inside the BAR0 window (BAR0 layout:
 * 2MiB MSI-X + 126MiB indirect CSR + 128MiB debug + 256MiB doorbell,
 * then 512MiB RCF). Mirrors PhalBridge::CHIP_RCF_BASE; defined here to
 * avoid a circular include (PhalBridge.h includes this header).
 * Every RCF-view address below is translated to a BAR0-window offset
 * by adding this constant - a config's base alone is NOT a BAR0 offset.
 */
constexpr uint32_t IPC_RCF_BAR0_OFFSET = 0x20000000u;

/*
 * Host-side mirror of TARGET_CORE_* in abios/common/ipc_rw/ipc_rw.h
 * (the diag tree deliberately does not include firmware headers).
 * PC(0) and HOST(1) are not selectable targets: HOST is the slot the
 * serving firmware polls for host-originated requests.
 */
enum class IpcFwId : uint32_t {
    Pmu = 2,   // TARGET_CORE_PMU
    Hqc = 3,   // TARGET_CORE_HQC
    Ddp0 = 4,  // TARGET_CORE_DDP0
    Ddp1 = 5,  // TARGET_CORE_DDP1
    Ddp2 = 6,  // TARGET_CORE_DDP2
    Ddp3 = 7,  // TARGET_CORE_DDP3
};

constexpr uint32_t IPC_FW_ID_FIRST = 2;
constexpr uint32_t IPC_FW_ID_LAST = 7;
constexpr size_t IPC_FW_COUNT = IPC_FW_ID_LAST - IPC_FW_ID_FIRST + 1;

const char* to_string(IpcFwId fw);

// Slot stride within an IPC region: 0x18 message + 4B trailing magic
// (mirror of IPC_CORE_STRIDE in ipc_rw.h).
constexpr uint32_t IPC_CORE_STRIDE = 0x1Cu;

// One IPC region description per firmware: base is the region's chip
// (RCF-view) address - NOT a BAR0-window offset; bind() adds
// IPC_RCF_BAR0_OFFSET to reach the BAR0 mapping. size is the window
// size, core_id is the slot index (slot = base + IPC_CORE_STRIDE*core_id).
// provisioned marks rows whose region actually exists in firmware: a
// false row is a placeholder whose address may be unmapped on canary
// / stale magic from another region, so neither the framework nor a
// testcase may touch it.
struct IpcRegionConfig {
    uint32_t base;
    uint32_t size;
    uint32_t core_id;
    bool provisioned;
};

// Single source of truth for per-fw region placement.
// TODO(per-fw): the Hqc row is assigned (HQC MCU view 0x16d00000 +
// 0x1F780 offset, host slot). PMU/DDP rows keep placeholder values
// (provisioned=false) until their firmware link scripts land dedicated
// regions; core_id moves off the host slot when a fw polls its own
// core slot.
constexpr IpcRegionConfig IPC_REGION_CONFIGS[IPC_FW_COUNT] = {
    /* Pmu  */ {0x1FF800u, 0x1000u, 1u, false},
    /* Hqc  */ {0x16D1F780u, 0x1000u, 1u, true},  // 0x16d00000 + 0x1F780
    /* Ddp0 */ {0x12C80720u, 0xE0u, 1u, true},
    /* Ddp1 */ {0x13C80720u, 0xE0u, 1u, true},
    /* Ddp2 */ {0x14C80720u, 0xE0u, 1u, true},
    /* Ddp3 */ {0x15C80720u, 0xE0u, 1u, true},
};

// Look up a fw's region config; out-of-range ids fall back to the Hqc row.
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
