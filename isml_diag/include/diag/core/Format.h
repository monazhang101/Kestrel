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
    for (size_t i = 0; i < count; ++i) {
        stream << std::setw(2) << static_cast<uint32_t>(bytes[i]);
    }
    return stream.str();
}

}
