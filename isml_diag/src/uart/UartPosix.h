#pragma once

#if defined(__linux__)
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace uart {
inline uint64_t monotonic_ms()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct FileDescriptor {
    int value = -1;
    explicit FileDescriptor(int fd = -1) : value(fd) {}
    ~FileDescriptor() { if (value >= 0) ::close(value); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
};
inline bool wait_fd(int fd, short events, uint64_t deadline)
{
    for (;;) {
        const auto now = monotonic_ms();
        if (now >= deadline) break;
        pollfd p{fd, events, 0};
        const auto remaining = deadline - now;
        const int rc = ::poll(&p, 1, int(std::min<uint64_t>(remaining, INT_MAX)));
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0 && (p.revents & events) != 0;
    }
    errno = ETIMEDOUT;
    return false;
}
inline bool socket_transfer(int fd, uint8_t* bytes, size_t size, bool sending, uint64_t deadline)
{
    size_t done = 0;
    while (done < size) {
        if (!wait_fd(fd, sending ? POLLOUT : POLLIN, deadline)) return false;
        const auto n = sending ? ::send(fd, bytes + done, size - done, MSG_NOSIGNAL)
                               : ::recv(fd, bytes + done, size - done, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}
} // namespace uart
#endif
