#pragma once

#include <cstddef>
#include <cstdint>

namespace uart {
inline void put_le(uint8_t* dst, uint64_t value, size_t count)
{
    for (size_t i = 0; i < count; ++i) dst[i] = uint8_t(value >> (8 * i));
}
inline uint64_t get_le(const uint8_t* src, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= uint64_t(src[i]) << (8 * i);
    return value;
}
}
