#pragma once

#include "diag/core/TestInfo.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ------------------------------------------------------------
// common::args::get_string() / common::args::get_u64() 
// provides typed argument parsing for test inputs.
// ------------------------------------------------------------
namespace common::args {

inline std::string get_string(const TestArgs& args,
                              const std::string& key,
                              const std::string& default_value)
{
    auto it = args.find(key);
    // Use the default when the caller did not provide this argument.
    return it == args.end() ? default_value : it->second;
}

inline uint64_t get_u64(const TestArgs& args,
                        const std::string& key,
                        uint64_t default_value)
{
    auto it = args.find(key);
    // Use the default when the caller did not provide this argument.
    if (it == args.end()) {
        return default_value;
    }

    // Pseudocode: use robust parsing and error reporting in real code.
    return static_cast<uint64_t>(std::stoull(it->second, nullptr, 0));
}

}

// -------------------------------------------------------------
// common::devmem::read() / common::devmem::write()
// copies byte ranges through a mapped device-memory BAR/window.
// It checks base/data/offset/len and falls back to byte copy when unaligned.
// -------------------------------------------------------------
namespace common::devmem {

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

// ---------------------------------------------
// common::reg::read() / common::reg::write()
// provides 32-bit aligned register range access helpers.
// ---------------------------------------------
namespace common::reg {

inline bool is_valid_range(uint64_t reg_size,
                           uint64_t reg_offset,
                           size_t len)
{
    // Reject empty transfers and offsets beyond the register window.
    if (len == 0 || reg_offset > reg_size) {
        return false;
    }

    // Reject ranges that would cross the register window boundary.
    return static_cast<uint64_t>(len) <= (reg_size - reg_offset);
}

inline bool is_aligned32(uintptr_t addr, size_t len)
{
    // Register helpers only allow 32-bit aligned accesses.
    return (addr % sizeof(uint32_t)) == 0 && (len % sizeof(uint32_t)) == 0;
}

inline bool read(void* reg_base,
                 uint64_t reg_size,
                 uint64_t reg_offset,
                 uint32_t* data,
                 size_t len)
{
    // Validate the mapped register base and output buffer.
    if (reg_base == nullptr || data == nullptr) {
        return false;
    }

    // Validate that the requested registers stay inside the block.
    if (!is_valid_range(reg_size, reg_offset, len)) {
        return false;
    }

    // Convert the register offset into an absolute mapped address.
    auto addr = reinterpret_cast<uintptr_t>(
        static_cast<uint8_t*>(reg_base) + reg_offset);

    // Reject unaligned register reads.
    if (!is_aligned32(addr, len)) {
        return false;
    }

    auto* src = reinterpret_cast<volatile uint32_t*>(addr);
    auto count = len / sizeof(uint32_t);
    // Read one 32-bit register at a time.
    for (size_t i = 0; i < count; ++i) {
        data[i] = src[i];
    }

    return true;
}

inline bool write(void* reg_base,
                  uint64_t reg_size,
                  uint64_t reg_offset,
                  const uint32_t* data,
                  size_t len)
{
    // Validate the mapped register base and input buffer.
    if (reg_base == nullptr || data == nullptr) {
        return false;
    }

    // Validate that the requested registers stay inside the block.
    if (!is_valid_range(reg_size, reg_offset, len)) {
        return false;
    }

    // Convert the register offset into an absolute mapped address.
    auto addr = reinterpret_cast<uintptr_t>(
        static_cast<uint8_t*>(reg_base) + reg_offset);

    // Reject unaligned register writes.
    if (!is_aligned32(addr, len)) {
        return false;
    }

    auto* dst = reinterpret_cast<volatile uint32_t*>(addr);
    auto count = len / sizeof(uint32_t);
    // Write one 32-bit register at a time.
    for (size_t i = 0; i < count; ++i) {
        dst[i] = data[i];
    }

    return true;
}

}

// -------------------------------------------------------------
// common::pattern::generate() / common::pattern::compare()
// generates and compares simple data payloads for DMA/devmem tests.
// Returns empty vector on error or unsupported pattern.
// -------------------------------------------------------------    
namespace common::pattern {

inline std::vector<uint8_t> generate(size_t size, const std::string& pattern)
{
    // Pseudocode: pattern payloads are generated for 32-bit data paths.
    // Reject empty or non-32-bit-sized payloads.
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
            // Pseudocode: deterministic pseudo-random bytes keep tests reproducible.
            seed = seed * 1664525u + 1013904223u;
            byte = static_cast<uint8_t>((seed >> 24) & 0xff);
        }
        return data;
    }

    return {};
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
