#ifndef __IPC_BRIDGE_H__
#define __IPC_BRIDGE_H__

/*
 * Host-side bridge between the diag framework and abios/common/ipc_rw.
 *
 * The firmware exposes the IPC message slots inside a BAR0 window:
 * the window maps IPC_BASE in chip address space, where each core owns
 * a |SEQ_NUM|OP_CODE|ADDR_LOW|ADDR_HIGH|ACK_NUM|VALUE| slot (0x18 bytes)
 * followed by a 4-byte interval magic (stride 0x1C). See ipc_rw.h.
 *
 * The host talks to the firmware through TARGET_CORE_HOST's slot:
 *   - host   : ISML_IPC_SEND (window base + slot) to post an RD/WR request,
 *              then polls ack_num == seq_num for the reply (value / error).
 *   - fw     : isml_target_exe_fn_routine() serves the slot, publishes
 *              value, then acks.
 *
 * The request/reply exchange is seq/ack based and single-slot, so one
 * transaction must complete before the next starts; IpcBridge guards
 * that with a mutex so multiple test threads can share one device.
 */

#include <cstdint>
#include <mutex>
#include <string>

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

// Chip address of the IPC region; the firmware's link script places the
// slots here and the host BAR0 window maps this address.
constexpr uint32_t IPC_REGION_BASE = 0x1FF800u;
constexpr uint32_t IPC_REGION_SIZE = 0x1000u;
constexpr uint32_t IPC_CORE_HOST_ID = 1; // TARGET_CORE_HOST

class IpcBridge {
public:
    IpcBridge() = default;
    ~IpcBridge() = default;
    IpcBridge(const IpcBridge&) = delete;
    IpcBridge& operator=(const IpcBridge&) = delete;

    /*
     * Bind the bridge to a device and map the IPC window from BAR0.
     * Maps a window of IPC_REGION_SIZE at IPC_REGION_BASE; returns false
     * and fills *error on any failure. Idempotent per device.
     */
    bool bind(const DeviceContext& ctx, std::string* error = nullptr);

    // True when bind() succeeded and the IPC window is mapped.
    bool ready() const { return window_ != nullptr; }

    /*
     * One RD/WR transaction against the firmware:
     *   - send the request to TARGET_CORE_HOST's slot,
     *   - poll ack_num == seq_num (bounded by poll_max spins),
     *   - on read store the firmware value into *value.
     * Returns IpcStatus::Ok on success; value carries the firmware's
     * error code on failure.
     */
    IpcStatus read(uint32_t addr, uint32_t& value,
                   uint32_t poll_max = default_poll_max());
    IpcStatus write(uint32_t addr, uint32_t value,
                    uint32_t poll_max = default_poll_max());

    static constexpr uint32_t default_poll_max() { return 1000000u; }

private:
    // Slot access within the mapped window, mirroring ipc_rw.h layout.
    volatile uint32_t* slot_() const;

    IpcStatus transact(IpcOp op, uint32_t addr, uint32_t data,
                       uint32_t& value_out, uint32_t poll_max);

    void* window_ = nullptr;     // mmap'd IPC region (BAR0-backed)
    uint64_t window_size_ = 0;   // mapped byte count
    std::mutex lock_;            // one in-flight transaction per bridge
};

#endif
