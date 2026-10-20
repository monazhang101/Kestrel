#include "diag/core/IpcBridge.h"

#include "diag/core/Common.h"
#include "diag/core/Platform.h"

namespace {

// Mirror of the ipc_rw.h slot layout: 6 uint32_t fields + 4B magic.
constexpr uint32_t IPC_CORE_STRIDE = 0x1Cu;

// Field indices inside one 6-word slot.
constexpr uint32_t SLOT_SEQ_NUM = 0;
constexpr uint32_t SLOT_OP_CODE = 1;
constexpr uint32_t SLOT_ADDR_LOW = 2;
constexpr uint32_t SLOT_ADDR_HIGH = 3;
constexpr uint32_t SLOT_ACK_NUM = 4;
constexpr uint32_t SLOT_VALUE = 5;

constexpr uint32_t SLOT_WORDS = 6;

// Compiler barrier: value/payload stores must be ordered before the
// seq_num doorbell store, and the ack poll must not be cached.
inline void ipc_compiler_barrier()
{
    asm volatile("" ::: "memory");
}

} // namespace

volatile uint32_t* IpcBridge::slot_() const
{
    // TARGET_CORE_HOST's slot: base + stride * core_id.
    auto* bytes = static_cast<uint8_t*>(window_);
    return reinterpret_cast<volatile uint32_t*>(
        bytes + IPC_CORE_STRIDE * IPC_CORE_HOST_ID);
}

bool IpcBridge::bind(const DeviceContext& ctx, std::string* error)
{
    if (error != nullptr) error->clear();

    const auto* bar = common::bar::find(ctx, 0);
    if (bar == nullptr || !bar->mapped) {
        if (error != nullptr) {
            *error = "IPC bind: BAR0 is not mapped (bdf=" + ctx.bdf + ")";
        }
        return false;
    }

    // The BAR0 window maps chip addresses starting at the BAR's device
    // base; the IPC region lives at IPC_REGION_BASE in that space.
    if (IPC_REGION_BASE + IPC_REGION_SIZE > bar->mapped_size) {
        if (error != nullptr) {
            *error = "IPC bind: BAR0 window too small for IPC region";
        }
        return false;
    }

    window_ = static_cast<uint8_t*>(bar->mapped_base) + IPC_REGION_BASE;
    window_size_ = IPC_REGION_SIZE;
    return true;
}

IpcStatus IpcBridge::transact(IpcOp op, uint32_t addr, uint32_t data,
                              uint32_t& value_out, uint32_t poll_max)
{
    std::lock_guard<std::mutex> guard(lock_);

    if (window_ == nullptr) {
        return IpcStatus::ErrInvalid;
    }

    volatile uint32_t* slot = slot_();

    // ---- ISML_IPC_SEND (window-mapped host variant) ----
    // Payload first, seq_num last (doorbell). seq = last_ack + 1.
    const uint32_t seq = slot[SLOT_ACK_NUM] + 1u;
    slot[SLOT_VALUE] = data;
    slot[SLOT_ADDR_HIGH] = 0;  // host addresses are 32-bit for now
    slot[SLOT_ADDR_LOW] = addr;
    slot[SLOT_OP_CODE] = static_cast<uint32_t>(op);
    ipc_compiler_barrier();
    slot[SLOT_SEQ_NUM] = seq;

    // ---- reply poll: wait until the firmware acks our seq ----
    uint32_t spins = poll_max;
    while (slot[SLOT_ACK_NUM] != seq) {
        if (spins-- == 0u) {
            return IpcStatus::ErrTimeout;
        }
    }

    // Ack reached: value is stable, the firmware will not touch the
    // slot again until the next send.
    value_out = slot[SLOT_VALUE];
    ipc_compiler_barrier();

    // The firmware reports handler failures through value = IPC_ERR_*.
    switch (value_out) {
    case static_cast<uint32_t>(IpcStatus::ErrInvalid):
        return IpcStatus::ErrInvalid;
    case static_cast<uint32_t>(IpcStatus::ErrUnknown):
        return IpcStatus::ErrUnknown;
    default:
        return IpcStatus::Ok;
    }
}

IpcStatus IpcBridge::read(uint32_t addr, uint32_t& value, uint32_t poll_max)
{
    uint32_t raw = 0;
    const auto status = transact(IpcOp::Read, addr, 0, raw, poll_max);
    if (status == IpcStatus::Ok) {
        value = raw;
    }
    return status;
}

IpcStatus IpcBridge::write(uint32_t addr, uint32_t value, uint32_t poll_max)
{
    uint32_t raw = 0;
    return transact(IpcOp::Write, addr, value, raw, poll_max);
}
