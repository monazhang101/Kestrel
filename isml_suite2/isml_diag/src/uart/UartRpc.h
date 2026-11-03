#pragma once

#include "diag/core/UartBridge.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace uart {

// Local RPC only; this format never reaches firmware. Fixed length, LE fields.
constexpr size_t RPC_SIZE = 40;
constexpr uint32_t RPC_MAGIC = 0x31545255u;
constexpr uint32_t MAX_CLIENTS = 16;
enum class RpcKind : uint16_t { Hello = 1, Read = 2, Write = 3, Reply = 4 };
struct RpcMessage {
    RpcKind kind = RpcKind::Hello;
    uint32_t id = 0;
    UartResult result = UartResult::Ok;
    uint64_t address = 0;
    uint32_t value = 0;
    uint32_t firmware_status = 0;
    uint32_t budget_ms = 0;
};

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
inline std::array<uint8_t, RPC_SIZE> encode_rpc(const RpcMessage& m)
{
    std::array<uint8_t, RPC_SIZE> b{};
    put_le(b.data(), RPC_MAGIC, 4); put_le(b.data() + 4, 1, 2);
    put_le(b.data() + 6, uint16_t(m.kind), 2); put_le(b.data() + 8, m.id, 4);
    put_le(b.data() + 12, uint32_t(m.result), 4);
    put_le(b.data() + 16, m.address, 8); put_le(b.data() + 24, m.value, 4);
    put_le(b.data() + 28, m.firmware_status, 4);
    put_le(b.data() + 32, m.budget_ms, 4);
    return b;
}
inline bool decode_rpc(const uint8_t* b, RpcMessage& m)
{
    if (get_le(b, 4) != RPC_MAGIC || get_le(b + 4, 2) != 1 ||
        get_le(b + 36, 4) != 0) return false;
    const auto kind = get_le(b + 6, 2), result = get_le(b + 12, 4);
    if (kind < 1 || kind > 4 || result > uint32_t(UartResult::Busy)) return false;
    m.kind = RpcKind(kind); m.id = uint32_t(get_le(b + 8, 4));
    m.result = UartResult(result); m.address = get_le(b + 16, 8);
    m.value = uint32_t(get_le(b + 24, 4));
    m.firmware_status = uint32_t(get_le(b + 28, 4));
    m.budget_ms = uint32_t(get_le(b + 32, 4));
    return true;
}

inline std::string socket_directory()
{
#if defined(__linux__)
    const auto* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] == '/') return std::string(runtime) + "/isml";
    return "/tmp/isml-uart-" + std::to_string(getuid());
#else
    return {};
#endif
}
inline std::string socket_path(const std::string& target)
{
    if (target.empty() || target.size() > 32) throw std::invalid_argument("invalid UART target");
    for (char c : target) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            throw std::invalid_argument("invalid UART target");
    }
    return socket_directory() + "/uart-" + target + ".sock";
}

} // namespace uart
