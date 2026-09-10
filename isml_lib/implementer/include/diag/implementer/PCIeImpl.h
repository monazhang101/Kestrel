#pragma once

#include "diag/core/HalContext.h"
#include "diag/core/TestInfo.h"

#include <cstdint>

struct DmaTransferRequest {
    DmaBuffer* host_buffer = nullptr;
    uint64_t host_offset = 0;
    uint64_t device_offset = 0;
    uint64_t size_bytes = 0;
    uint64_t timeout_ms = 0;
};

class PCIeImpl {
public:
    virtual ~PCIeImpl() = default;

    virtual TestStatus bar_read32(TestInfo& ti) = 0;
    virtual TestStatus bar_scan32(TestInfo& ti) = 0;
    virtual TestStatus dma_copy_h2d(TestInfo& ti, const DmaTransferRequest& req) = 0;
    virtual TestStatus dma_copy_d2h(TestInfo& ti, const DmaTransferRequest& req) = 0;
};
