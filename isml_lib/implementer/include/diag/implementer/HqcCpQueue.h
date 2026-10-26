#pragma once

#include "diag/implementer/PCIeImpl.h"

// Atlas CP fetch engine, parallel to HqcDmaEngine. Defaults to MEM3/IDX7;
// the environment must exclude the selected queue from FW/admin tests.
// Calls are synchronous and serialized per physical device. Caller keeps both
// buffers alive; uncertain completion invalidates and retains their handles.
class HqcCpQueue {
public:
    explicit HqcCpQueue(HalContext& hal) : hal_(hal) {}

    TestStatus copy(const CpTransferRequest& request);
    TestStatus copy(DmaBuffer& src, MemBuffer& dst, uint64_t bytes,
                    uint64_t timeout_ms = 1000,
                    uint64_t src_offset = 0, uint64_t dst_offset = 0);
    TestStatus copy(DmaBuffer& src, MemBuffer& dst, uint64_t bytes,
                    CpQueueId queue, uint64_t timeout_ms = 1000,
                    uint64_t src_offset = 0, uint64_t dst_offset = 0);
    TestStatus copy(MemBuffer& src, DmaBuffer& dst, uint64_t bytes,
                    uint64_t timeout_ms = 1000,
                    uint64_t src_offset = 0, uint64_t dst_offset = 0);
    TestStatus copy(MemBuffer& src, DmaBuffer& dst, uint64_t bytes,
                    CpQueueId queue, uint64_t timeout_ms = 1000,
                    uint64_t src_offset = 0, uint64_t dst_offset = 0);
    TestStatus copy(MemBuffer& src, MemBuffer& dst, uint64_t bytes,
                    uint64_t timeout_ms = 1000,
                    uint64_t src_offset = 0, uint64_t dst_offset = 0);
    TestStatus copy(MemBuffer& src, MemBuffer& dst, uint64_t bytes,
                    CpQueueId queue, uint64_t timeout_ms = 1000,
                    uint64_t src_offset = 0, uint64_t dst_offset = 0);

private:
    HalContext& hal_;
};
