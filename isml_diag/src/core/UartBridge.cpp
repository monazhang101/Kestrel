#include "diag/core/UartBridge.h"
#include "uart/UartRpc.h"
#include "uart/UartPosix.h"

#include <limits>

#if defined(__linux__)
#include <cstring>
#include <sys/un.h>
#endif

UartBridge::~UartBridge() { disconnect(); }
void UartBridge::disconnect()
{
#if defined(__linux__)
    if (fd_ >= 0) ::close(fd_);
#endif
    fd_ = -1;
}
bool UartBridge::bind(const std::string& target, std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    disconnect(); error_.clear(); firmware_status_ = 0; next_id_ = 0;
#if defined(__linux__)
    try {
        const auto path = uart::socket_path(target);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) throw std::invalid_argument("UART socket path is too long");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) throw std::runtime_error("cannot create UART client socket");
        const auto deadline = uart::monotonic_ms() + 2000;
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            if (errno != EINPROGRESS || !uart::wait_fd(fd_, POLLOUT, deadline))
                throw std::runtime_error("UART console unavailable at " + path + ": " + std::strerror(errno));
            int status = 0; socklen_t size = sizeof(status);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &status, &size) < 0 || status != 0)
                throw std::runtime_error("UART console connection failed at " + path);
        }
        std::array<uint8_t, uart::RPC_SIZE> bytes{};
        uart::RpcMessage hello;
        if (!uart::socket_transfer(fd_, bytes.data(), bytes.size(), false, deadline) ||
            !uart::decode_rpc(bytes.data(), hello) || hello.kind != uart::RpcKind::Hello ||
            hello.id != 0 || hello.result != UartResult::Ok || hello.budget_ms == 0)
            throw std::runtime_error("UART console did not provide a valid greeting");
        budget_ms_ = hello.budget_ms;
        return true;
    } catch (const std::exception& e) { error_ = e.what(); disconnect(); }
#else
    (void)target;
    error_ = "UART console transport requires Linux";
#endif
    if (error) *error = error_;
    return false;
}
UartResult UartBridge::transact(bool writing, uint64_t address, uint32_t& value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    firmware_status_ = 0; error_.clear();
    if (address % 4 != 0) { error_ = "UART address must be 32-bit aligned"; return UartResult::Invalid; }
    if (fd_ < 0) { error_ = "UART console is not connected"; return UartResult::Transport; }
#if defined(__linux__)
    if (next_id_ == std::numeric_limits<uint32_t>::max()) {
        error_ = "UART local request IDs exhausted"; disconnect(); return UartResult::Transport;
    }
    uart::RpcMessage request;
    request.kind = writing ? uart::RpcKind::Write : uart::RpcKind::Read;
    request.id = ++next_id_; request.address = address; request.value = writing ? value : 0;
    auto bytes = uart::encode_rpc(request);
    const auto deadline = uart::monotonic_ms() + budget_ms_;
    uart::RpcMessage reply;
    if (!uart::socket_transfer(fd_, bytes.data(), bytes.size(), true, deadline) ||
        !uart::socket_transfer(fd_, bytes.data(), bytes.size(), false, deadline) ||
        !uart::decode_rpc(bytes.data(), reply) || reply.kind != uart::RpcKind::Reply || reply.id != request.id) {
        error_ = "UART console disconnected, timed out, or returned invalid RPC; command outcome may be unknown";
        disconnect(); return UartResult::Transport;
    }
    firmware_status_ = reply.firmware_status;
    if (reply.result == UartResult::Ok && !writing) value = reply.value;
    if (reply.result != UartResult::Ok) {
        switch (reply.result) {
        case UartResult::Timeout:
            error_ = "no matching response after configured retries; command outcome may be unknown"; break;
        case UartResult::Invalid: error_ = "invalid UART request"; break;
        case UartResult::Firmware: error_ = "firmware rejected or failed the UART command"; break;
        case UartResult::Busy: error_ = "UART console is busy"; break;
        default: error_ = "UART transport failed; command outcome may be unknown"; break;
        }
        error_ += " (firmware_status=" + std::to_string(reply.firmware_status) + ")";
    }
    return reply.result;
#else
    (void)writing; (void)value;
    return UartResult::Transport;
#endif
}
UartResult UartBridge::read(uint64_t address, uint32_t& value) { return transact(false, address, value); }
UartResult UartBridge::write(uint64_t address, uint32_t value) { return transact(true, address, value); }
