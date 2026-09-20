#pragma once

#include "diag/core/Platform.h"
#include "diag/core/TestInfo.h"
#include "diag/core/DevMem.h"
#include "diag/core/Format.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// -------------------------------------------------------------
// common::mmio::read() / common::mmio::write()
// is the lowest-level entry for all mapped device IO in this framework.
// It copies checked byte ranges through an already mapped MMIO window.
// -------------------------------------------------------------
namespace common::mmio {

inline bool is_valid_range(uint64_t dev_mem_size,
                           uint64_t dev_mem_offset,
                           size_t len)
{
    // Reject empty transfers and offsets beyond the mapped window.
    if (len == 0 || dev_mem_offset > dev_mem_size) {
        return false;
    }

    // Reject ranges that would cross the mapped window boundary.
    return static_cast<uint64_t>(len) <= (dev_mem_size - dev_mem_offset);
}

inline bool is_aligned32(uintptr_t addr, const void* data, size_t len)
{
    // Fast 32-bit access requires device address, host buffer, and length alignment.
    return (addr % sizeof(uint32_t)) == 0 &&
           (reinterpret_cast<uintptr_t>(data) % sizeof(uint32_t)) == 0 &&
           (len % sizeof(uint32_t)) == 0;
}

inline bool read(void* dev_mem_base,
                 uint64_t dev_mem_size,
                 uint64_t dev_mem_offset,
                 void* data,
                 size_t len)
{
    // Validate the mapped base, host buffer, and requested byte range.
    if (dev_mem_base == nullptr || data == nullptr ||
        !is_valid_range(dev_mem_size, dev_mem_offset, len)) {
        return false;
    }

    // Convert the window offset into an absolute mapped address.
    auto addr = reinterpret_cast<uintptr_t>(
        static_cast<uint8_t*>(dev_mem_base) + dev_mem_offset);

    // Use 32-bit volatile reads when both sides are aligned.
    if (is_aligned32(addr, data, len)) {
        auto* src = reinterpret_cast<volatile uint32_t*>(addr);
        auto* dst = static_cast<uint32_t*>(data);
        auto count = len / sizeof(uint32_t);

        // Copy one 32-bit word at a time from the mapped device window.
        for (size_t i = 0; i < count; ++i) {
            dst[i] = src[i];
        }
        return true;
    }

    // Fall back to byte access for unaligned ranges or host buffers.
    auto* src = reinterpret_cast<volatile uint8_t*>(addr);
    auto* dst = static_cast<uint8_t*>(data);
    // Copy one byte at a time from the mapped device window.
    for (size_t i = 0; i < len; ++i) {
        dst[i] = src[i];
    }

    return true;
}

inline bool write(void* dev_mem_base,
                  uint64_t dev_mem_size,
                  uint64_t dev_mem_offset,
                  const void* data,
                  size_t len)
{
    // Validate the mapped base, host buffer, and requested byte range.
    if (dev_mem_base == nullptr || data == nullptr ||
        !is_valid_range(dev_mem_size, dev_mem_offset, len)) {
        return false;
    }

    // Convert the window offset into an absolute mapped address.
    auto addr = reinterpret_cast<uintptr_t>(
        static_cast<uint8_t*>(dev_mem_base) + dev_mem_offset);

    // Use 32-bit volatile writes when both sides are aligned.
    if (is_aligned32(addr, data, len)) {
        auto* dst = reinterpret_cast<volatile uint32_t*>(addr);
        auto* src = static_cast<const uint32_t*>(data);
        auto count = len / sizeof(uint32_t);

        // Copy one 32-bit word at a time into the mapped device window.
        for (size_t i = 0; i < count; ++i) {
            dst[i] = src[i];
        }
        return true;
    }

    // Fall back to byte access for unaligned ranges or host buffers.
    auto* dst = reinterpret_cast<volatile uint8_t*>(addr);
    auto* src = static_cast<const uint8_t*>(data);
    // Copy one byte at a time into the mapped device window.
    for (size_t i = 0; i < len; ++i) {
        dst[i] = src[i];
    }

    return true;
}

}

// -------------------------------------------------------------
// common::bar is the public raw BAR access shared by every module.
// It selects one mapped PCI BAR from DeviceContext and delegates all
// data movement to common::mmio.
// -------------------------------------------------------------
namespace common::bar {

inline const BarMapping* find(const DeviceContext& ctx, uint32_t bar_index)
{
    for (const auto& bar : ctx.bar_mappings) {
        if (bar.bar_index == bar_index) {
            return &bar;
        }
    }
    return nullptr;
}

inline void* mapped_base(const DeviceContext& ctx, uint32_t bar_index)
{
    const auto* bar = find(ctx, bar_index);
    if (bar == nullptr || !bar->mapped) {
        return nullptr;
    }
    return bar->mapped_base;
}

inline uint64_t mapped_size(const DeviceContext& ctx, uint32_t bar_index)
{
    const auto* bar = find(ctx, bar_index);
    if (bar == nullptr || !bar->mapped) {
        return 0;
    }
    return bar->mapped_size;
}

inline bool read(const DeviceContext& ctx,
                 uint32_t bar_index,
                 uint64_t offset,
                 void* data,
                 size_t len)
{
    const auto* bar = find(ctx, bar_index);
    if (bar == nullptr || !bar->mapped) {
        return false;
    }
    return common::mmio::read(bar->mapped_base, bar->mapped_size, offset, data, len);
}

inline bool write(const DeviceContext& ctx,
                  uint32_t bar_index,
                  uint64_t offset,
                  const void* data,
                  size_t len)
{
    const auto* bar = find(ctx, bar_index);
    if (bar == nullptr || !bar->mapped || !bar->writable) {
        return false;
    }
    return common::mmio::write(bar->mapped_base, bar->mapped_size, offset, data, len);
}

inline bool read32(const DeviceContext& ctx,
                   uint32_t bar_index,
                   uint64_t offset,
                   uint32_t& value)
{
    return read(ctx, bar_index, offset, &value, sizeof(value));
}

inline bool write32(const DeviceContext& ctx,
                    uint32_t bar_index,
                    uint64_t offset,
                    uint32_t value)
{
    return write(ctx, bar_index, offset, &value, sizeof(value));
}

}

// Module-relative 32-bit access. PHAL callbacks continue using common::bar
// directly because env.base + addr is already a complete BAR offset.
namespace common {

inline TestStatus block_ctx_enter(DeviceContext& ctx, uint64_t offset)
{
    if (offset % 4 != 0 || ctx.block_offset > ctx.reg_size ||
        offset > ctx.reg_size - ctx.block_offset) return TestStatus::INVALID;
    ctx.block_offset += offset;
    return TestStatus::OK;
}

inline TestStatus block_ctx_exit(DeviceContext& ctx, uint64_t offset)
{
    if (offset % 4 != 0 || offset > ctx.block_offset) return TestStatus::INVALID;
    ctx.block_offset -= offset;
    return TestStatus::OK;
}

namespace detail {
inline bool reg_address(const DeviceContext& ctx, uint64_t addr, uint64_t& absolute)
{
    if (addr % 4 != 0 || ctx.block_offset % 4 != 0 || ctx.reg_base_offset % 4 != 0 ||
        ctx.block_offset > ctx.reg_size || addr > ctx.reg_size - ctx.block_offset)
        return false;
    const auto relative = ctx.block_offset + addr;
    if (ctx.reg_size - relative < sizeof(uint32_t) ||
        ctx.reg_base_offset > std::numeric_limits<uint64_t>::max() - relative)
        return false;
    absolute = ctx.reg_base_offset + relative;
    return true;
}

inline TestStatus reg_result(const DeviceContext& ctx, const char* operation,
                             uint64_t addr, uint64_t absolute, uint32_t value,
                             TestStatus status)
{
    if (ctx.logger != nullptr) {
        const auto message = std::string(operation) + " target=" + ctx.target_name +
            " module_base=" + format::hex(ctx.reg_base_offset) +
            " block_offset=" + format::hex(ctx.block_offset) +
            " reg_offset=" + format::hex(addr) +
            " bar_offset=" + format::hex(absolute) +
            " value=" + format::hex(value, 8) + " status=" + test_status_name(status);
        if (status == TestStatus::OK) ctx.logger->info(message);
        else ctx.logger->error(message);
    }
    return status;
}
}

inline TestStatus reg_write(DeviceContext& ctx, uint64_t addr, uint32_t data)
{
    uint64_t absolute = 0;
    auto status = TestStatus::INVALID;
    if (detail::reg_address(ctx, addr, absolute)) {
        status = bar::write32(ctx, ctx.reg_bar_index, absolute, data)
                     ? TestStatus::OK : TestStatus::ERROR;
    }
    return detail::reg_result(ctx, "reg_write", addr, absolute, data, status);
}

inline TestStatus reg_read(DeviceContext& ctx, uint64_t addr, uint32_t* data)
{
    uint64_t absolute = 0;
    uint32_t value = 0;
    auto status = TestStatus::INVALID;
    if (data != nullptr && detail::reg_address(ctx, addr, absolute)) {
        status = bar::read32(ctx, ctx.reg_bar_index, absolute, value)
                     ? TestStatus::OK : TestStatus::ERROR;
        if (status == TestStatus::OK) *data = value;
    }
    return detail::reg_result(ctx, "reg_read", addr, absolute, value, status);
}
}

using common::block_ctx_enter;
using common::block_ctx_exit;
using common::reg_read;
using common::reg_write;

// -------------------------------------------------------------
// BAR-path device-memory read/write shared by every module lives in
// DevMem.h. common::devmem programs a PHAL PCIe aperture before it
// accesses the selected data BAR through common::bar.
// -------------------------------------------------------------

// ------------------------------------------------------------
// common::args::get_string() / common::args::get_u64()
// reads inputs after TestTarget has applied registered defaults and validated
// argument formats and policy ranges.
// ------------------------------------------------------------
namespace common::args {

inline std::string get_string(const TestArgs& args,
                              const std::string& key)
{
    auto it = args.find(key);
    if (it == args.end()) {
        throw std::invalid_argument("missing required argument: " + key);
    }
    return it->second;
}

inline uint64_t get_u64(const TestArgs& args,
                        const std::string& key)
{
    auto value = get_string(args, key);

    try {
        return static_cast<uint64_t>(std::stoull(value, nullptr, 0));
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid u64 argument: " + key + "=" + value);
    }
}

inline uint64_t get_u64(const TestArgs& args,
                        const std::string& key,
                        uint64_t default_value)
{
    auto it = args.find(key);
    if (it == args.end()) {
        return default_value;
    }

    try {
        return static_cast<uint64_t>(std::stoull(it->second, nullptr, 0));
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid u64 argument: " + key + "=" + it->second);
    }
}

}

// -------------------------------------------------------------
// common::pattern::generate() / common::pattern::compare()
// generates and compares simple data payloads for DMA/window tests.
// Returns empty vector on error or unsupported pattern.
// -------------------------------------------------------------    
namespace common::pattern {

inline std::vector<uint8_t> generate(size_t size, const std::string& pattern)
{
    // Pattern payloads use the 32-bit transfer granularity required by DMA tests.
    if (size == 0 || (size % sizeof(uint32_t)) != 0) {
        return {};
    }

    std::vector<uint8_t> data(size, 0);

    // The zero pattern is already produced by vector initialization.
    if (pattern == "zero") {
        return data;
    }

    // Fill the payload with a deterministic incremental byte pattern.
    if (pattern == "incremental") {
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<uint8_t>(i & 0xff);
        }
        return data;
    }

    // Fill the payload with a deterministic pseudo-random byte pattern.
    if (pattern == "random") {
        uint32_t seed = 0x12345678;
        for (auto& byte : data) {
            // Deterministic pseudo-random bytes keep test failures reproducible.
            seed = seed * 1664525u + 1013904223u;
            byte = static_cast<uint8_t>((seed >> 24) & 0xff);
        }
        return data;
    }

    return {};
}

inline bool write(void* dst, size_t size, const std::string& pattern)
{
    if (dst == nullptr) {
        return false;
    }

    auto data = generate(size, pattern);
    if (data.empty()) {
        return false;
    }

    std::memcpy(dst, data.data(), data.size());
    return true;
}

inline bool compare(const void* expected,
                    const void* actual,
                    size_t size)
{
    if (expected == nullptr || actual == nullptr || size == 0) {
        return false;
    }
    return std::memcmp(expected, actual, size) == 0;
}

inline bool compare(const std::vector<uint8_t>& expected,
                    const std::vector<uint8_t>& actual)
{
    // Empty payloads are treated as invalid comparisons.
    if (expected.empty() || actual.empty()) {
        return false;
    }
    return expected == actual;
}

}
