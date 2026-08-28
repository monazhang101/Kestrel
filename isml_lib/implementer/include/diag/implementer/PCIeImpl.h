#pragma once

#include "diag/core/HalBackend.h"
#include "diag/core/TestInfo.h"

#include <cstdint>
#include <string>

struct LinkStatus {
    bool ok = false;
    std::string current_speed;
    std::string current_width;
    std::string error;
    TestMetrics metrics;
};

struct DmaTransferRequest {
    DmaBuffer* host_buffer = nullptr;
    uint64_t host_offset = 0;
    uint64_t device_offset = 0;
    uint64_t size_bytes = 0;
    uint64_t timeout_ms = 0;
};

struct DmaTransferResult {
    bool ok = false;
    uint64_t bytes = 0;
    uint64_t duration_us = 0;
    std::string completion_status;
    std::string error;
    TestMetrics metrics;
};

class PCIeImpl {
public:
    virtual ~PCIeImpl() = default;

    virtual LinkStatus link_status_get() = 0;
    virtual TestResult bar_read32(TestInfo& ti) = 0;
    virtual TestResult bar_scan32(TestInfo& ti) = 0;
    virtual DmaTransferResult dma_copy_h2d(const DmaTransferRequest& req) = 0;
    virtual DmaTransferResult dma_copy_d2h(const DmaTransferRequest& req) = 0;
};
