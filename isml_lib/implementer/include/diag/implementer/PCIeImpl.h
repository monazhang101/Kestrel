#pragma once

#include "diag/core/HalContext.h"
#include "diag/core/TestInfo.h"

#include <cstdint>

// Semantic paths, not FW wire values. The product implementation maps the ABI.
enum class DmaTransferType {
    H2D, D2H, D2I, I2D, D2S, S2D, D2V, V2D,
};

struct DmaTransferRequest {
    DmaTransferType type;
    // Required only for H2D/D2H; owned by the caller throughout the transfer.
    DmaBuffer* host_buffer = nullptr;
    // Byte offsets in the spaces selected by type. A host endpoint is relative
    // to host_buffer; the implementation translates it to a DMA address.
    uint64_t src_offset = 0;
    uint64_t dst_offset = 0;
    uint64_t size_bytes = 0;
    uint64_t timeout_ms = 0;
};

class PCIeImpl {
public:
    virtual ~PCIeImpl() = default;

    virtual TestStatus bar_read32(TestInfo& ti) = 0;
    virtual TestStatus bar_scan32(TestInfo& ti) = 0;
    // Caller selects a platform-reserved scratch range. Descriptor validation
    // does not discover physical capacity or reserve device memory.
    virtual TestStatus dma_copy(TestInfo& ti, const DmaTransferRequest& req) = 0;
};
