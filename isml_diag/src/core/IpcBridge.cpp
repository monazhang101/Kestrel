#include "diag/core/IpcBridge.h"

#include "diag/core/Common.h"
#include "diag/core/Platform.h"

#include <chrono>

namespace {

// Mirror of the ipc_rw.h slot layout: interval magic stamped by the
// firmware's isml_ipc_init() between core slots.
constexpr uint32_t IPC_INTERVAL_MAGIC = 0x46494D47u;

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

const char* to_string(IpcFwId fw)
{
    switch (fw) {
    case IpcFwId::Pmu:  return "pmu";
    case IpcFwId::Hqc:  return "hqc";
    case IpcFwId::Ddp0: return "ddp0";
    case IpcFwId::Ddp1: return "ddp1";
    case IpcFwId::Ddp2: return "ddp2";
    case IpcFwId::Ddp3: return "ddp3";
    }
    return "unknown";
}

volatile uint32_t* IpcBridge::slot_() const
{
    // Bound core's slot: region base + stride * core_id.
    auto* bytes = static_cast<uint8_t*>(window_);
    return reinterpret_cast<volatile uint32_t*>(
        bytes + IPC_CORE_STRIDE * core_id_);
}

bool IpcBridge::bind(const DeviceContext& ctx, IpcFwId fw, std::string* error)
{
    return bind(ctx, fw, ipc_region_config(fw), error);
}

bool IpcBridge::bind(const DeviceContext& ctx, IpcFwId fw,
                     const IpcRegionConfig& cfg, std::string* error)
{
    if (error != nullptr) error->clear();

    if (!cfg.provisioned) {
        if (error != nullptr) {
            using common::format::hex;
            *error = "IPC bind: region is a placeholder, not provisioned "
                     "(rcf_base=" + hex(cfg.base) + "); fill the row in "
                     "IPC_REGION_CONFIGS when the fw ships a real region";
        }
        return false;
    }

    const auto* bar = common::bar::find(ctx, 0);
    if (bar == nullptr || !bar->mapped) {
        if (error != nullptr) {
            *error = "IPC bind: BAR0 is not mapped (bdf=" + ctx.bdf + ")";
        }
        return false;
    }

    // cfg.base is an RCF-view chip address; the region lives in the RCF
    // portion of the BAR0 window (BAR0 layout:
    // 2MiB MSI-X + 126MiB indirect CSR + 128MiB debug + 256MiB
    // doorbell, then 512MiB RCF). Translate before any bounds check.
    const uint64_t bar0_offset =
        static_cast<uint64_t>(IPC_RCF_BAR0_OFFSET) + cfg.base;
    if (bar0_offset + cfg.size > bar->mapped_size) {
        if (error != nullptr) {
            using common::format::hex;
            *error = "IPC bind: IPC region outside BAR0 window (fw=" +
                     std::string(to_string(fw)) +
                     " rcf_base=" + hex(cfg.base) +
                     " bar0_offset=" + hex(bar0_offset) +
                     " mapped_size=" + hex(bar->mapped_size) + ")";
        }
        return false;
    }

    window_ = static_cast<uint8_t*>(bar->mapped_base) + bar0_offset;
    window_size_ = cfg.size;
    bar0_offset_ = bar0_offset;
    region_base_ = cfg.base;
    region_size_ = cfg.size;
    core_id_ = cfg.core_id;
    return true;
}

bool IpcBridge::check_magic(const DeviceContext& ctx, std::string* error) const
{
    if (error != nullptr) error->clear();

    if (window_ == nullptr) {
        if (error != nullptr) {
            *error = "IPC magic check: bridge is not bound";
        }
        return false;
    }

    // The first interval magic sits at the end of the PC slot; it is
    // stamped by the firmware's isml_ipc_init(). Read it through the raw
    // BAR0 path (not the seq/ack slot) so the check involves no handshake.
    uint32_t magic = 0;
    if (!common::bar::read32(ctx, 0, bar0_offset_ + 0x18u, magic)) {
        if (error != nullptr) {
            *error = "IPC magic check: failed to read magic at bar0_offset=" +
                     common::format::hex(bar0_offset_ + 0x18u);
        }
        return false;
    }
    if (magic != IPC_INTERVAL_MAGIC) {
        if (error != nullptr) {
            *error = "IPC magic check: expected 0x46494D47, actual=" +
                     common::format::hex(magic, 8) +
                     " (is the firmware running and did it call "
                     "isml_ipc_init()?)";
        }
        return false;
    }
    return true;
}

IpcStatus IpcBridge::transact(IpcOp op, uint32_t addr, uint32_t data,
                              uint32_t& value_out, uint32_t poll_max,
                              uint32_t timeout_ms)
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
    // Two bounds: poll_max spins and a wall-clock deadline. Spin count
    // alone is unreliable - an MMIO ack read over a slow link can take
    // microseconds each, stretching poll_max spins to minutes. The
    // wall-clock deadline guarantees the caller gets control back.
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms == 0 ? 1u : timeout_ms);
    uint32_t spins = poll_max;
    while (slot[SLOT_ACK_NUM] != seq) {
        if (spins-- == 0u) {
            // No ack within poll_max spins: the bound firmware is not
            // serving the slot. Common causes: firmware not running /
            // isml_ipc_init() not called, or the region config points to
            // the window at the wrong address.
            return IpcStatus::ErrTimeout;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // Deadline fired first: same diagnosis as the spin guard,
            // but reported with the elapsed bound.
            return IpcStatus::ErrTimeout;
        }
    }

    // Ack reached: value is stable, the firmware will not touch the
    // slot again until the next send.
    value_out = slot[SLOT_VALUE];
    ipc_compiler_barrier();

    // The firmware reports handler failures through value = IPC_ERR_*.
    /*switch (value_out) {
    case static_cast<uint32_t>(IpcStatus::ErrInvalid):
        return IpcStatus::ErrInvalid;
    case static_cast<uint32_t>(IpcStatus::ErrUnknown):
        return IpcStatus::ErrUnknown;
    default:
        return IpcStatus::Ok;
    }*/
    return IpcStatus::Ok;
}

IpcStatus IpcBridge::read(uint32_t addr, uint32_t& value, uint32_t poll_max)
{
    uint32_t raw = 0;
    const auto status = transact(IpcOp::Read, addr, 0, raw, poll_max,
                                 default_timeout_ms());
    if (status == IpcStatus::Ok) {
        value = raw;
    }
    return status;
}

IpcStatus IpcBridge::write(uint32_t addr, uint32_t value, uint32_t poll_max)
{
    uint32_t raw = 0;
    return transact(IpcOp::Write, addr, value, raw, poll_max,
                    default_timeout_ms());
}

IpcStatus to_ipc_status_text(IpcStatus status, IpcFwId fw, std::string& text)
{
    using common::format::hex;
    const char* fw_name = to_string(fw);
    switch (status) {
    case IpcStatus::ErrTimeout:
        text = std::string(fw_name) + " ipc timeout: firmware not serving "
               "the slot (not running, isml_ipc_init() not called, or "
               "region/base mismatch)";
        return status;
    case IpcStatus::ErrInvalid:
        text = std::string(fw_name) + " ipc invalid request";
        return status;
    case IpcStatus::ErrUnknown:
        text = std::string(fw_name) + " ipc unknown opcode/handler";
        return status;
    case IpcStatus::Ok:
        text = std::string(fw_name) + " ipc ok";
        return status;
    }
    text = std::string(fw_name) + " ipc unknown status";
    return status;
}
