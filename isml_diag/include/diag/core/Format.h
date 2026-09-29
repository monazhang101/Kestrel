#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace common::format {

inline std::string hex(uint64_t value, size_t width = 0)
{
    std::ostringstream stream;
    stream << "0x" << std::hex;
    if (width != 0) {
        stream << std::setw(static_cast<int>(width)) << std::setfill('0');
    }
    stream << value;
    return stream.str();
}

inline std::string hex_bytes(const void* data,
                             size_t len,
                             size_t max_bytes = 8)
{
    if (data == nullptr || len == 0 || max_bytes == 0) {
        return "0x";
    }

    const auto* bytes = static_cast<const uint8_t*>(data);
    const auto count = std::min(len, max_bytes);
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setfill('0');
    // Print as little-endian words (highest address first) so a 4-byte value
    // of 0x12345678 previews as "0x12345678" instead of "0x78563412".
    constexpr size_t WORD_BYTES = 4;
    size_t word_end = count;
    while (word_end > 0) {
        const auto word_begin = word_end >= WORD_BYTES ? word_end - WORD_BYTES : 0;
        for (size_t i = word_end; i > word_begin; --i) {
            stream << std::setw(2) << static_cast<uint32_t>(bytes[i - 1]);
        }
        word_end = word_begin;
    }
    return stream.str();
}

}
