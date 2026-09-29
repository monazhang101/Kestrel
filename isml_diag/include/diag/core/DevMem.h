/* Common access to DMEM through a PCIe aperture and RCF memory through BAR0. */
#pragma once

#include "diag/core/MemoryRegion.h"
#include "diag/core/TestInfo.h"
#include "diag/core/Platform.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct DeviceContext;
class HalContext;

namespace common {
// Owns an allocated interval. Keep it within the device/HAL lifetime and wait
// for DMA completion before release. Separate allocations never overlap.
class MemBuffer {
    // Borrowed owner used by mem_read/write; the buffer must not outlive its
    // HalContext. The allocation interval itself remains shared in ctx_.
    HalContext* alloc_ctx_ = nullptr;
    std::unique_ptr<DeviceContext> ctx_;
    MemoryRegion region_ = DMEM;
    uint64_t offset_ = 0;
    uint64_t size_ = 0;
    TestStatus transfer(uint64_t offset,
                        void* output, const void* input, size_t len);
    friend class ::HalContext;
    friend TestStatus mem_read(MemBuffer&, uint64_t, uint32_t*);
    friend TestStatus mem_write(MemBuffer&, uint64_t, uint32_t);
    friend TestStatus mem_read(MemBuffer&, uint64_t, std::vector<uint8_t>*);
    friend TestStatus mem_write(MemBuffer&, uint64_t, const std::vector<uint8_t>&);
public:
    MemBuffer() = default;
    ~MemBuffer() { release(); }
    MemBuffer(const MemBuffer&) = delete;
    MemBuffer& operator=(const MemBuffer&) = delete;
    MemBuffer(MemBuffer&& other) noexcept;
    MemBuffer& operator=(MemBuffer&& other) noexcept;
    bool valid() const { return alloc_ctx_ != nullptr && ctx_ != nullptr; }
    uint64_t offset() const { return offset_; } // Byte offset within the region.
    uint64_t size() const { return valid() ? size_ : 0; }
    void release();
    // Failed asynchronous IO may still use this range. Keep it occupied until
    // device teardown instead of returning it to the allocator.
    void keep_allocated() { alloc_ctx_ = nullptr; ctx_.reset(); }
};

TestStatus mem_read(MemBuffer& buffer, uint64_t offset, uint32_t* data);
TestStatus mem_write(MemBuffer& buffer, uint64_t offset, uint32_t data);
TestStatus mem_read(MemBuffer& buffer, uint64_t offset, std::vector<uint8_t>* data);
TestStatus mem_write(MemBuffer& buffer, uint64_t offset, const std::vector<uint8_t>& data);

}

using common::mem_read;
using common::mem_write;
using common::MemBuffer;
