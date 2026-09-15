#pragma once

#include "diag/core/BaseDevice.h"

#include <cstddef>
#include <cstdint>

class Logger;

namespace atlas_impl {

constexpr int HQC_STATUS_OK = 0;
constexpr int HQC_STATUS_TIMEOUT = 1;
constexpr int HQC_STATUS_IO_ERROR = 2;
constexpr int HQC_QUEUE_INVALID = -1;
constexpr int HQC_QUEUE_FULL = -2;
constexpr int HQC_QUEUE_EMPTY = -3;

// Atlas FW admin-command ABI values copied from the bring-up command format.
// These are wire values, not framework-local enum ordinals; replace them with
// a shared versioned FW header when one is available.
constexpr uint8_t HQC_ADMIN_CMD_TEST = 8;
constexpr uint32_t HQC_TEST_DMA_HOST_TO_DMEM = 6;
constexpr uint32_t HQC_TEST_DMA_DMEM_TO_HOST = 7;

#pragma pack(push, 1)
struct HqcAdminHeader {
    uint8_t cmd_type;
    uint8_t cmd_id;
    uint8_t cmd_status;
    uint8_t reserved;
};

struct HqcAdminDma {
    uint64_t src_offset;
    uint64_t dst_offset;
    uint32_t size;
    uint32_t reserved;
};

struct HqcAdminTest {
    uint32_t test_type;
    union {
        HqcAdminDma dma;
        uint32_t reserved[6];
    } submit;
};

struct HqcAdminCommand {
    HqcAdminHeader header;
    union {
        uint32_t user_data[15];
        HqcAdminTest test;
    } payload;
};
#pragma pack(pop)

static_assert(sizeof(HqcAdminCommand) == 64, "HQC admin command ABI must be 64 bytes");
static_assert(offsetof(HqcAdminCommand, payload.test.submit.dma.src_offset) == 8,
              "HQC DMA source field ABI mismatch");
static_assert(offsetof(HqcAdminCommand, payload.test.submit.dma.dst_offset) == 16,
              "HQC DMA destination field ABI mismatch");
static_assert(offsetof(HqcAdminCommand, payload.test.submit.dma.size) == 24,
              "HQC DMA size field ABI mismatch");

class HqcAdminQueue {
public:
    // Full BAR0-relative SRAM offset, including the PCIe TOP base.
    HqcAdminQueue(const DeviceContext& device,
                  uint64_t hqc_sram_bar_offset,
                  Logger* logger);

    int full(bool& is_full) const;
    int empty(bool& is_empty) const;
    int push(const HqcAdminCommand& command, uint64_t timeout_ms) const;
    int pop(HqcAdminCommand& command, uint64_t timeout_ms) const;

private:
    struct Queue {
        uint64_t buffer;
        uint64_t wptr;
        uint64_t rptr;
    };

    const DeviceContext& device_;
    uint64_t hqc_sram_bar_offset_;
    Logger* logger_ = nullptr;

    // Atlas core-0 bring-up queue layout, expressed as byte offsets inside HQC
    // SRAM. Each queue owns 0x400 bytes (16 commands x 64 bytes); pointer
    // registers contain monotonically increasing byte counters. Replace these
    // constants with the shared FW/HQC memory-map definitions when available.
    // TODO: select by core id and add cmd_id correlation before concurrent or
    // queue-saturation tests. The MVP intentionally keeps one FIFO request.
    static constexpr uint64_t CORE0_SQ_BUFFER_OFFSET = 0x1ff80;
    static constexpr uint64_t CORE0_SQ_WPTR_OFFSET = 0x20f80;
    static constexpr uint64_t CORE0_SQ_RPTR_OFFSET = 0x20f84;
    static constexpr uint64_t CORE0_CQ_BUFFER_OFFSET = 0x20380;
    static constexpr uint64_t CORE0_CQ_WPTR_OFFSET = 0x20f88;
    static constexpr uint64_t CORE0_CQ_RPTR_OFFSET = 0x20f8c;

    const Queue sq_{CORE0_SQ_BUFFER_OFFSET,
                    CORE0_SQ_WPTR_OFFSET,
                    CORE0_SQ_RPTR_OFFSET};
    const Queue cq_{CORE0_CQ_BUFFER_OFFSET,
                    CORE0_CQ_WPTR_OFFSET,
                    CORE0_CQ_RPTR_OFFSET};

    int read32(uint64_t offset, uint32_t& value) const;
    int write32(uint64_t offset, uint32_t value) const;
};

}
