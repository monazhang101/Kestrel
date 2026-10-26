#pragma once

#include "diag/core/HalContext.h"
#include "diag/core/TestInfo.h"

#include <cstdint>

// Semantic paths, not FW wire values. The product implementation maps the ABI.
enum class DmaTransferType {
    H2D, D2H,
    D2I, I2D,
    D2S, S2D,
    D2V, V2D,
    H2I, I2H,
    H2S, S2H,
};

struct DmaTransferRequest {
    DmaTransferType type;
    // Required when either endpoint is host memory; owned by the caller
    // throughout the transfer.
    DmaBuffer* host_buffer = nullptr;
    // Byte offsets in the spaces selected by type. A host endpoint is relative
    // to host_buffer; the implementation translates it to a DMA address.
    uint64_t src_offset = 0;
    uint64_t dst_offset = 0;
    uint64_t size_bytes = 0;
    uint64_t timeout_ms = 0;
};

// SRAM means HQC SRAM (all four banks), never AIC SMEM. Semantic enum values
// are mapped to the CP direction wire encoding by the product backend.
enum class CpTransferType { H2Sram, Sram2H, D2Sram, Sram2D };
// COMP0..3 are global CP queue IDX0..3; MEM0..3 are IDX4..7.
// Queue selection does not imply concurrent submission.
enum class CpQueueId { Comp0, Comp1, Comp2, Comp3, Mem0, Mem1, Mem2, Mem3 };

struct CpTransferRequest {
    CpTransferType type = CpTransferType::H2Sram;
    DmaBuffer* host_buffer = nullptr;
    MemBuffer* dmem_buffer = nullptr;
    MemBuffer* sram_buffer = nullptr;
    // Byte offsets relative to the source/destination allocations.
    uint64_t src_offset = 0;
    uint64_t dst_offset = 0;
    uint64_t size_bytes = 0;
    uint64_t timeout_ms = 1000;
    CpQueueId queue = CpQueueId::Mem3;
};

class PCIeImpl {
public:
    virtual ~PCIeImpl() = default;

    // Caller selects a platform-reserved scratch range. Descriptor validation
    // does not discover physical capacity or reserve device memory.
    virtual TestStatus dma_copy(TestInfo& ti, const DmaTransferRequest& req) = 0;
    // Synchronous one-shot copy. On uncertain completion the backend retains
    // allocations and disables the queue; callers must not reuse the buffers.
    virtual TestStatus cp_copy(TestInfo& ti, const CpTransferRequest& req) = 0;
};
