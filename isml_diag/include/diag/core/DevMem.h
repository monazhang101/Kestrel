/* Common access to DMEM through a PCIe aperture and RCF memory through BAR0. */
#pragma once

#include "diag/core/MemoryRegion.h"
#include "diag/core/TestInfo.h"
#include "diag/core/Platform.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DeviceContext;
struct TestInfo;

namespace common::devmem {

struct Window {
    uint32_t bar_index = 0;
    uint8_t aperture_index = 0;
    uint8_t identity = 0;
    uint64_t target_addr = 0;
    uint64_t size = 0;
    uint64_t bar_offset = 0;
};

bool read(TestInfo& ti,
          const DeviceContext& ctx,
          const Window& window,
          uint64_t offset,
          void* data,
          size_t len,
          std::string* error = nullptr);

bool write(TestInfo& ti,
           const DeviceContext& ctx,
           const Window& window,
           uint64_t offset,
           const void* data,
           size_t len,
           std::string* error = nullptr);

}

namespace common {
// Owns an allocated interval. Keep it within the device/HAL lifetime and wait
// for DMA completion before release. Separate allocations never overlap.
class MemBuffer {
    std::unique_ptr<DeviceContext> ctx_;
    MemoryRegion region_ = DMEM;
    uint64_t offset_ = 0;
    uint64_t size_ = 0;
    TestStatus transfer(uint64_t offset, void* output, const void* input, size_t len);
    friend MemBuffer mem_alloc(const DeviceContext&, MemoryRegion, uint64_t);
    friend TestStatus mem_read(MemBuffer&, uint64_t, uint32_t*);
    friend TestStatus mem_write(MemBuffer&, uint64_t, uint32_t);
    friend TestStatus mem_read(MemBuffer&, uint64_t, std::vector<uint8_t>*);
    friend TestStatus mem_write(MemBuffer&, uint64_t, const std::vector<uint8_t>&);
public:
    MemBuffer() = default;
    ~MemBuffer() { release(); }
    MemBuffer(const MemBuffer&) = delete;
    MemBuffer& operator=(const MemBuffer&) = delete;
    MemBuffer(MemBuffer&&) noexcept = default;
    MemBuffer& operator=(MemBuffer&& other) noexcept;
    bool valid() const { return ctx_ != nullptr; }
    uint64_t offset() const { return offset_; } // Byte offset within the region.
    uint64_t size() const { return valid() ? size_ : 0; }
    void release();
    // Failed asynchronous IO may still use this range. Keep it occupied until
    // device teardown instead of returning it to the allocator.
    void keep_allocated() { ctx_.reset(); }
};

MemBuffer mem_alloc(const DeviceContext& ctx, MemoryRegion region, uint64_t size);
TestStatus mem_read(MemBuffer& buffer, uint64_t offset, uint32_t* data);
TestStatus mem_write(MemBuffer& buffer, uint64_t offset, uint32_t data);
TestStatus mem_read(MemBuffer& buffer, uint64_t offset, std::vector<uint8_t>* data);
TestStatus mem_write(MemBuffer& buffer, uint64_t offset, const std::vector<uint8_t>& data);

// Low-level fixed-address access for firmware protocols and mapping tests.
TestStatus mem_read(DeviceContext& ctx, MemoryRegion region,
                    uint64_t addr, uint32_t* data);
TestStatus mem_write(DeviceContext& ctx, MemoryRegion region,
                     uint64_t addr, uint32_t data);
TestStatus mem_read(DeviceContext& ctx, MemoryRegion region,
                    uint64_t addr, std::vector<uint8_t>* data);
TestStatus mem_write(DeviceContext& ctx, MemoryRegion region,
                     uint64_t addr, const std::vector<uint8_t>& data);
inline TestStatus mem_read(DeviceContext& ctx, MemoryRegion region,
                           uint64_t addr, std::nullptr_t)
{
    return mem_read(ctx, region, addr, static_cast<uint32_t*>(nullptr));
}
}

using common::mem_read;
using common::mem_write;
using common::mem_alloc;
using common::MemBuffer;
