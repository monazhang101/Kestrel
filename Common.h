#pragma once

#include "BaseDevice.h"

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
    return it == args.end() ? default_value : it->second;
}

inline uint64_t get_u64(const TestArgs& args,
                        const std::string& key,
                        uint64_t default_value)
{
    auto it = args.find(key);
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
    if (len == 0 || dev_mem_offset > dev_mem_size) {
        return false;
    }

    return static_cast<uint64_t>(len) <= (dev_mem_size - dev_mem_offset);
}

inline bool read(void* dev_mem_base,
                 uint64_t dev_mem_size,
                 uint64_t dev_mem_offset,
                 void* data,
                 size_t len)
{
    if (dev_mem_base == nullptr || data == nullptr ||
        !is_valid_range(dev_mem_size, dev_mem_offset, len)) {
        return false;
    }

    auto addr = reinterpret_cast<uintptr_t>(
        static_cast<uint8_t*>(dev_mem_base) + dev_mem_offset);

    if ((addr % sizeof(uint32_t)) == 0 && (len % sizeof(uint32_t)) == 0) {
        auto* src = reinterpret_cast<volatile uint32_t*>(addr);
        auto* dst = static_cast<uint32_t*>(data);
        auto count = len / sizeof(uint32_t);

        for (size_t i = 0; i < count; ++i) {
            dst[i] = src[i];
        }
        return true;
    }

    auto* src = reinterpret_cast<volatile uint8_t*>(addr);
    auto* dst = static_cast<uint8_t*>(data);
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
    if (dev_mem_base == nullptr || data == nullptr ||
        !is_valid_range(dev_mem_size, dev_mem_offset, len)) {
        return false;
    }

    auto addr = reinterpret_cast<uintptr_t>(
        static_cast<uint8_t*>(dev_mem_base) + dev_mem_offset);

    if ((addr % sizeof(uint32_t)) == 0 && (len % sizeof(uint32_t)) == 0) {
        auto* dst = reinterpret_cast<volatile uint32_t*>(addr);
        auto* src = static_cast<const uint32_t*>(data);
        auto count = len / sizeof(uint32_t);

        for (size_t i = 0; i < count; ++i) {
            dst[i] = src[i];
        }
        return true;
    }

    auto* dst = reinterpret_cast<volatile uint8_t*>(addr);
    auto* src = static_cast<const uint8_t*>(data);
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
    if (len == 0 || reg_offset > reg_size) {
        return false;
    }

    return static_cast<uint64_t>(len) <= (reg_size - reg_offset);
}

inline bool is_aligned32(uintptr_t addr, size_t len)
{
    return (addr % sizeof(uint32_t)) == 0 && (len % sizeof(uint32_t)) == 0;
}

inline bool read(void* reg_base,
                 uint64_t reg_size,
                 uint64_t reg_offset,
                 uint32_t* data,
                 size_t len)
{
    if (reg_base == nullptr || data == nullptr) {
        return false;
    }

    if (!is_valid_range(reg_size, reg_offset, len)) {
        return false;
    }

    auto addr = reinterpret_cast<uintptr_t>(
        static_cast<uint8_t*>(reg_base) + reg_offset);

    if (!is_aligned32(addr, len)) {
        return false;
    }

    auto* src = reinterpret_cast<volatile uint32_t*>(addr);
    auto count = len / sizeof(uint32_t);
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
    if (reg_base == nullptr || data == nullptr) {
        return false;
    }

    if (!is_valid_range(reg_size, reg_offset, len)) {
        return false;
    }

    auto addr = reinterpret_cast<uintptr_t>(
        static_cast<uint8_t*>(reg_base) + reg_offset);

    if (!is_aligned32(addr, len)) {
        return false;
    }

    auto* dst = reinterpret_cast<volatile uint32_t*>(addr);
    auto count = len / sizeof(uint32_t);
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
    if (size == 0 || (size % sizeof(uint32_t)) != 0) {
        return {};
    }

    std::vector<uint8_t> data(size, 0);

    if (pattern == "zero") {
        return data;
    }

    if (pattern == "incremental") {
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<uint8_t>(i & 0xff);
        }
        return data;
    }

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
    if (expected.empty() || actual.empty()) {
        return false;
    }
    return expected == actual;
}

}
