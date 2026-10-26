#include "diag/implementer/HqcCpQueue.h"
#include "diag/core/Common.h"
#include "diag/implementer/HqcDma.h"

extern "C" {
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/PCIE.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/HQC/HQC.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/HQC/PCIE_HQC_REG/PCIE_HQC_REG.h>
}

#include <atomic>
#include <chrono>
#include <limits>
#include <thread>

namespace {
using common::format::hex;
using Clock = std::chrono::steady_clock;

constexpr uint64_t ALIGNMENT = 128;
constexpr uintptr_t DOORBELL_BASE = 0x10000000;
constexpr uint32_t COMP_WINDOW_BEGIN = 0x02000000;
constexpr uint32_t MEM_WINDOW_BEGIN = 0x08000000;
constexpr uint32_t DOORBELL_SIZE = 0x10000000;
constexpr uint32_t QUEUE_STRIDE = 0x1000;
constexpr uint64_t SRAM_BANK_SIZE = 0x80000;

// FW CP queue wire layout, shared by the read-only canonical image and the
// writable doorbell alias. These are byte offsets within ONE queue, not
// generated CSR register names: a generator may pack multiple words into a
// single register. Only the canonical image base comes from the CSR header.
constexpr uint32_t DIRECTION = 0;
constexpr uint32_t DOORBELL = 0x04;
constexpr uint32_t CREDIT = 0x0c;
constexpr uint32_t SRC_LOW = 0x10;
constexpr uint32_t SRC_HIGH = 0x14;
constexpr uint32_t SRC_SIZE = 0x18;
constexpr uint32_t DST_LOW = 0x1c;
constexpr uint32_t DST_HIGH = 0x20;
constexpr uint32_t DST_SIZE = 0x24;
constexpr uint32_t WPTR = 0x28;
constexpr uint32_t RPTR = 0x2c;
constexpr uint32_t CANONICAL_STRIDE = RPTR + sizeof(uint32_t);

struct QueueInfo {
    bool memory;
    uint32_t index;
    uint32_t state_index;
    uint32_t remap_csr;
    uint32_t default_remap;
    uint32_t window_begin;
    uint32_t window_end;
    uint32_t canonical_base;
    uint32_t credit_depth;
    const char* name;
};

bool queue_info(CpQueueId id, QueueInfo& info)
{
    const auto value = static_cast<uint32_t>(id);
    if (value >= 8) return false;
    static constexpr const char* NAMES[] = {
        "COMP0", "COMP1", "COMP2", "COMP3",
        "MEM0", "MEM1", "MEM2", "MEM3"
    };
    info.memory = value >= 4;
    info.index = value & 3u;
    info.state_index = value;
    info.remap_csr = (info.memory ? PCIE_HQC_REG_MEM_CP_QUEUE_0_OFFSET
                                  : PCIE_HQC_REG_COMP_CP_QUEUE_0_OFFSET) +
                     info.index * sizeof(uint32_t);
    info.window_begin = info.memory ? MEM_WINDOW_BEGIN : COMP_WINDOW_BEGIN;
    info.window_end = info.memory ? DOORBELL_SIZE : MEM_WINDOW_BEGIN;
    info.default_remap = info.window_begin + 0x4000 + info.index * QUEUE_STRIDE;
    info.canonical_base = info.memory
        ? PCIE_HQC_REG_MEM_O_QUEUE_0_REG_FETCH_DIRECTION_OFFSET
        : PCIE_HQC_REG_COMPUTE_O_QUEUE_0_REG_FETCH_DIRECTION_OFFSET;
    info.credit_depth = info.memory
        ? PCIE_HQC_REG_MEM_O_QUEUE_0_REG_DB_CRD_DEFAULT
        : PCIE_HQC_REG_COMPUTE_O_QUEUE_0_REG_DB_CRD_DEFAULT;
    info.name = NAMES[value];
    return true;
}

int sram_bank(MemoryRegion region)
{
    switch (region) {
    case HQC_SRAM_0: return 0;
    case HQC_SRAM_1: return 1;
    case HQC_SRAM_2: return 2;
    case HQC_SRAM_3: return 3;
    default: return -1;
    }
}

bool address(uint64_t base, uint64_t capacity, uint64_t offset,
             uint64_t bytes, uint64_t& result)
{
    if (offset > capacity || bytes > capacity - offset ||
        offset > UINT64_MAX - base) return false;
    result = base + offset;
    return result % ALIGNMENT == 0 && bytes - 1 <= UINT64_MAX - result;
}

// Guard even exceptional exits after a potentially posted doorbell write.
// No reset/cancel contract exists: poison the shared queue and retain buffers.
struct SubmissionGuard {
    bool& faulted;
    const CpTransferRequest& request;
    bool touched = false;
    bool kicked = false;
    bool completed = false;
    bool cleaned = false;
    ~SubmissionGuard()
    {
        if (touched && !cleaned) faulted = true;
        if (kicked && !completed) {
            if (request.host_buffer) request.host_buffer->keep_allocated();
            if (request.dmem_buffer) request.dmem_buffer->keep_allocated();
            request.sram_buffer->keep_allocated();
        }
    }
};
}

TestStatus HqcCpQueue::copy(DmaBuffer& src, MemBuffer& dst, uint64_t bytes,
                          uint64_t timeout, uint64_t so, uint64_t dso)
{
    return copy(src, dst, bytes, CpQueueId::Mem3, timeout, so, dso);
}

TestStatus HqcCpQueue::copy(DmaBuffer& src, MemBuffer& dst, uint64_t bytes,
                          CpQueueId queue, uint64_t timeout,
                          uint64_t so, uint64_t dso)
{
    CpTransferRequest request{
        CpTransferType::H2Sram, &src, nullptr, &dst, so, dso, bytes, timeout};
    request.queue = queue;
    return copy(request);
}

TestStatus HqcCpQueue::copy(MemBuffer& src, DmaBuffer& dst, uint64_t bytes,
                          uint64_t timeout, uint64_t so, uint64_t dso)
{
    return copy(src, dst, bytes, CpQueueId::Mem3, timeout, so, dso);
}

TestStatus HqcCpQueue::copy(MemBuffer& src, DmaBuffer& dst, uint64_t bytes,
                          CpQueueId queue, uint64_t timeout,
                          uint64_t so, uint64_t dso)
{
    CpTransferRequest request{
        CpTransferType::Sram2H, &dst, nullptr, &src, so, dso, bytes, timeout};
    request.queue = queue;
    return copy(request);
}

TestStatus HqcCpQueue::copy(MemBuffer& src, MemBuffer& dst, uint64_t bytes,
                          uint64_t timeout, uint64_t so, uint64_t dso)
{
    return copy(src, dst, bytes, CpQueueId::Mem3, timeout, so, dso);
}

TestStatus HqcCpQueue::copy(MemBuffer& src, MemBuffer& dst, uint64_t bytes,
                          CpQueueId queue, uint64_t timeout,
                          uint64_t so, uint64_t dso)
{
    CpTransferRequest request;
    if (src.region() == DMEM)
        request = {CpTransferType::D2Sram, nullptr, &src, &dst,
                   so, dso, bytes, timeout};
    else
        request = {CpTransferType::Sram2D, nullptr, &dst, &src,
                   so, dso, bytes, timeout};
    request.queue = queue;
    return copy(request);
}

TestStatus HqcCpQueue::copy(const CpTransferRequest& req)
{
    if (!hal_.has_device()) return PHAL_STATUS_INVALID;
    const auto& device = hal_.device_context();
    auto* logger = device.logger;
    const auto fail = [&](TestStatus status, const std::string& message) {
        if (logger) logger->error("CP copy: " + message);
        return status;
    };
    if (device.tpu_type != TPUType::Atlas) return PHAL_STATUS_UNIMPLEMENTED;
    QueueInfo queue_info_value{};
    if (!queue_info(req.queue, queue_info_value))
        return fail(PHAL_STATUS_INVALID, "invalid queue selector");
    if (!device.memory || req.size_bytes == 0 || req.size_bytes % ALIGNMENT ||
        req.size_bytes > UINT32_MAX || req.timeout_ms == 0 ||
        req.timeout_ms > static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count() / 2))
        return fail(PHAL_STATUS_INVALID, "invalid size_bytes or timeout_ms (length must be a multiple of 128)");

    bool host = false, to_sram = false;
    uint32_t direction = 0;
    const char* name = nullptr;
    switch (req.type) {
    case CpTransferType::H2Sram: host = true; to_sram = true; direction = 0; name = "h2sram"; break;
    case CpTransferType::Sram2H: host = true; direction = 1; name = "sram2h"; break;
    case CpTransferType::D2Sram: to_sram = true; direction = 2; name = "d2sram"; break;
    case CpTransferType::Sram2D: direction = 3; name = "sram2d"; break;
    default: return fail(PHAL_STATUS_INVALID, "unsupported direction");
    }
    if (!req.sram_buffer || !req.sram_buffer->valid() ||
        !req.sram_buffer->belongs_to(hal_))
        return fail(PHAL_STATUS_INVALID, "HQC SRAM buffer must belong to this HAL");
    const int bank = sram_bank(req.sram_buffer->region());
    if (bank < 0 || req.sram_buffer->offset() > SRAM_BANK_SIZE ||
        req.sram_buffer->size() > SRAM_BANK_SIZE - req.sram_buffer->offset())
        return fail(PHAL_STATUS_INVALID, "expected an HQC SRAM bank allocation");

    uint64_t sram_addr = 0, other_addr = 0;
    if (!address(ATL_HQC_SRAM_MCU_LOCAL_BASE +
                     static_cast<uint64_t>(bank) * SRAM_BANK_SIZE +
                     req.sram_buffer->offset(), req.sram_buffer->size(),
                 to_sram ? req.dst_offset : req.src_offset, req.size_bytes, sram_addr))
        return fail(PHAL_STATUS_INVALID, "HQC SRAM range or 128-byte address alignment is invalid");
    const auto other_offset = to_sram ? req.src_offset : req.dst_offset;
    if (host) {
        if (req.dmem_buffer || !req.host_buffer || !req.host_buffer->valid() ||
            !req.host_buffer->belongs_to(hal_) ||
            !address(req.host_buffer->device_addr(), req.host_buffer->size(),
                     other_offset, req.size_bytes, other_addr))
            return fail(PHAL_STATUS_INVALID, "host buffer, range or 128-byte address alignment is invalid");
    } else {
        if (req.host_buffer || !req.dmem_buffer || !req.dmem_buffer->valid() ||
            !req.dmem_buffer->belongs_to(hal_) || req.dmem_buffer->region() != DMEM ||
            !address(req.dmem_buffer->address(), req.dmem_buffer->size(),
                     other_offset, req.size_bytes, other_addr))
            return fail(PHAL_STATUS_INVALID, "DMEM buffer, range or 128-byte address alignment is invalid");
    }
    const uint64_t src = to_sram ? other_addr : sram_addr;
    const uint64_t dst = to_sram ? sram_addr : other_addr;
    auto& state = *device.memory;
    std::lock_guard<std::mutex> queue_lock(state.cp_queue_mutex);
    auto& queue_faulted = state.cp_queue_faulted[queue_info_value.state_index];
    if (queue_faulted)
        return fail(PHAL_STATUS_ERROR, std::string(queue_info_value.name) +
            " is faulted; reset/quiesce the device before reopening the session");

    SubmissionGuard submission{queue_faulted, req};
    const auto deadline = Clock::now() + std::chrono::milliseconds(req.timeout_ms);
    uint32_t remap = 0, credit = 0, wptr = 0, rptr = 0;
    const char* phase = "initialize";
    if (logger) logger->info(std::string("CP copy: direction=") + name +
        " queue=" + queue_info_value.name + " src=" + hex(src) + " dst=" + hex(dst) +
        " size_bytes=" + std::to_string(req.size_bytes) +
        " timeout_ms=" + std::to_string(req.timeout_ms));

    const auto status = hal_.with_pcie_context([&](phal_ctx_t* ctx) -> TestStatus {
        // with_pcie_context already holds the shared PCIe IO lock. Switch the
        // PHAL base between RCF CSR and the doorbell alias without adding RCF's
        // 0x20000000 translation to the doorbell window.
        const auto saved_base = ctx->env.base;
        struct Restore { phal_ctx_t* ctx; uintptr_t base; ~Restore() { ctx->env.base = base; } } restore{ctx, saved_base};
        const uintptr_t csr = saved_base + PCIE_U_HQC_0_OFFSET + HQC_U_PCIE_HQC_REG_0_OFFSET;
        const uintptr_t canonical = csr + queue_info_value.canonical_base +
                                    queue_info_value.index * CANONICAL_STRIDE;
        auto read = [&](uintptr_t base, uint32_t offset, uint32_t& value) {
            ctx->env.base = base;
            return phal_read(ctx, offset, &value);
        };
        auto write = [&](uintptr_t base, uint32_t offset, uint32_t value) {
            ctx->env.base = base;
            submission.touched = true;
            return phal_write(ctx, offset, value);
        };
        auto poll = [&](auto condition) -> TestStatus {
            for (;;) {
                if (Clock::now() >= deadline) return PHAL_STATUS_TIMEOUT;
                bool done = false;
                const auto s = condition(done);
                if (s != PHAL_STATUS_OK) return s;
                if (done) return PHAL_STATUS_OK;
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        };
        auto drain = [&](uintptr_t base) {
            return poll([&](bool& done) -> TestStatus {
                auto s = read(base, CREDIT, credit);
                if (s != PHAL_STATUS_OK) return s;
                if (credit > queue_info_value.credit_depth)
                    return PHAL_STATUS_ERROR;
                s = read(base, WPTR, wptr);
                if (s != PHAL_STATUS_OK) return s;
                s = read(base, RPTR, rptr);
                done = credit == queue_info_value.credit_depth && wptr == rptr;
                return s;
            });
        };

        auto s = read(csr, queue_info_value.remap_csr, remap);
        if (s != PHAL_STATUS_OK) return s;
        phase = "wait idle";
        s = drain(canonical);
        if (s != PHAL_STATUS_OK) return s;
        if (!host) {
            uint32_t config = 0;
            s = read(csr, PCIE_HQC_REG_HQC_Q128_CONFIG_OFFSET, config);
            if (s != PHAL_STATUS_OK) return s;
            if ((config & 1u) == 0)
                return fail(PHAL_STATUS_ERROR, "Q128 disabled; configure the platform DMEM translation first");
        }
        const bool initialize_remap = remap == 0;
        if (initialize_remap) remap = queue_info_value.default_remap;
        if (remap < queue_info_value.window_begin ||
            remap > queue_info_value.window_end - QUEUE_STRIDE ||
            remap % QUEUE_STRIDE)
            return fail(PHAL_STATUS_INVALID, std::string(queue_info_value.name) +
                " remap is outside its doorbell region or not 4KiB aligned");
        // Another queue aliasing this group would violate exclusive ownership.
        for (uint32_t i = 0; i < 4; ++i) {
            uint32_t other = 0;
            if (!queue_info_value.memory || i != queue_info_value.index) {
                s = read(csr, PCIE_HQC_REG_MEM_CP_QUEUE_0_OFFSET + i * 4, other);
                if (s != PHAL_STATUS_OK) return s;
                if (other == remap) return PHAL_STATUS_INVALID;
            }
            if (queue_info_value.memory || i != queue_info_value.index) {
                s = read(csr, PCIE_HQC_REG_COMP_CP_QUEUE_0_OFFSET + i * 4, other);
                if (s != PHAL_STATUS_OK) return s;
                if (other == remap) return PHAL_STATUS_INVALID;
            }
        }
        if (initialize_remap) {
            // Validate placement/collisions before changing the remap CSR.
            // Do not touch other queues or global controls.
            s = write(csr, queue_info_value.remap_csr, remap);
            if (s != PHAL_STATUS_OK) return s;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            s = read(csr, queue_info_value.remap_csr, remap);
            if (s != PHAL_STATUS_OK) return s;
            if (remap != queue_info_value.default_remap) return PHAL_STATUS_ERROR;
        }
        const uintptr_t queue = DOORBELL_BASE + remap;
        if (logger) logger->debug("CP registers: csr_base=" + hex(csr) +
            " remap=" + hex(remap) + " queue_bar0=" + hex(queue));
        auto reset_pointers = [&]() -> TestStatus {
            auto r = write(queue, WPTR, 0);
            if (r != PHAL_STATUS_OK) return r;
            r = write(queue, RPTR, 0);
            if (r != PHAL_STATUS_OK) return r;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            r = read(queue, WPTR, wptr); // Same-device read completes posted writes.
            if (r != PHAL_STATUS_OK) return r;
            r = read(queue, RPTR, rptr);
            if (r != PHAL_STATUS_OK) return r;
            return wptr == 0 && rptr == 0 ? PHAL_STATUS_OK : PHAL_STATUS_ERROR;
        };
        phase = "configure";
        s = reset_pointers();
        if (s != PHAL_STATUS_OK) return s;
        // Bit[2] remains zero as in FW polling mode. No host interrupt route or
        // sync flag is configured. FW must not consume this dedicated queue.
        const std::pair<uint32_t, uint32_t> fields[] = {
            {DIRECTION, direction},
            {SRC_LOW, static_cast<uint32_t>(src)}, {SRC_HIGH, static_cast<uint32_t>(src >> 32)},
            {SRC_SIZE, static_cast<uint32_t>(req.size_bytes)},
            {DST_LOW, static_cast<uint32_t>(dst)}, {DST_HIGH, static_cast<uint32_t>(dst >> 32)},
            {DST_SIZE, static_cast<uint32_t>(req.size_bytes)}
        };
        for (const auto& field : fields) {
            s = write(queue, field.first, field.second);
            if (s != PHAL_STATUS_OK) return s;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Read back configuration before triggering; never kick after a
        // rejected/masked configuration write.
        for (const auto& field : fields) {
            uint32_t value = 0;
            s = read(queue, field.first, value);
            if (s != PHAL_STATUS_OK) return s;
            if (value != field.second) return PHAL_STATUS_ERROR;
        }
        s = read(queue, CREDIT, credit);
        if (s != PHAL_STATUS_OK) return s;
        if (credit != queue_info_value.credit_depth) return PHAL_STATUS_ERROR;
        if (Clock::now() >= deadline) return PHAL_STATUS_TIMEOUT;
        phase = "completion";
        std::atomic_thread_fence(std::memory_order_seq_cst);
        submission.kicked = true; // A failed write may already have reached HW.
        s = write(queue, DOORBELL, 1);
        if (s != PHAL_STATUS_OK) return s;
        s = poll([&](bool& done) -> TestStatus {
            const auto r = read(queue, WPTR, wptr);
            // Pointer baseline is zero after reconfig. uint32 subtraction is
            // modular; do not use credit restoration as payload completion.
            done = static_cast<uint32_t>(wptr - rptr) >= req.size_bytes;
            return r;
        });
        if (s != PHAL_STATUS_OK) return s;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        submission.completed = true;
        if (logger) logger->debug(std::string("CP payload complete: queue=") +
            queue_info_value.name + " wptr=" +
            hex(wptr) + " rptr=" + hex(rptr) + " transferred_bytes=" +
            std::to_string(static_cast<uint32_t>(wptr - rptr)));
        phase = "acknowledge";
        s = write(queue, RPTR, wptr);
        if (s != PHAL_STATUS_OK) return s;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        phase = "reconfig";
        s = drain(queue);
        if (s != PHAL_STATUS_OK) return s;
        s = reset_pointers();
        if (s == PHAL_STATUS_OK) submission.cleaned = true;
        return s;
    });
    if (status != PHAL_STATUS_OK) {
        if (submission.touched) queue_faulted = true;
        return fail(status, std::string("phase=") + phase + " remap=" + hex(remap) +
            " credit=" + std::to_string(credit) + " wptr=" + hex(wptr) + " rptr=" + hex(rptr) +
            (submission.kicked && !submission.completed
                ? "; queue disabled, buffers retained; reset/quiesce before process exit" : ""));
    }
    if (logger) logger->info(std::string("CP copy completed: direction=") + name +
                            " queue=" + queue_info_value.name +
                            " size_bytes=" + std::to_string(req.size_bytes));
    return status;
}
