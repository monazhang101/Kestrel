#include "UartConsole.h"
#include "UartPosix.h"
#include "UartRpc.h"

#include <stdexcept>

#if defined(__linux__)
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <limits>
#include <optional>
#include <random>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>

namespace uart {
namespace {
constexpr size_t MAX_OUTPUT = 65536;
constexpr uint32_t SERIAL_WRITE_TIMEOUT_MS = 1000;

std::runtime_error system_error(const std::string& action)
{
    return std::runtime_error(action + ": " + std::strerror(errno));
}
speed_t baud_speed(uint32_t baud)
{
    switch (baud) {
    case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
    case 57600: return B57600; case 115200: return B115200;
    case 230400: return B230400; case 460800: return B460800; case 921600: return B921600;
    default: throw std::invalid_argument("unsupported UART baud rate");
    }
}
struct SerialPort {
    FileDescriptor fd;
    termios original{};
    bool configured = false;
    bool exclusive = false;
    explicit SerialPort(const ConsoleConfig& config)
        : fd(::open(config.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC))
    {
        if (fd.value < 0) throw system_error("open UART " + config.port);
        if (::flock(fd.value, LOCK_EX | LOCK_NB) < 0) throw system_error("UART already owned");
        if (::ioctl(fd.value, TIOCEXCL) < 0) throw system_error("claim exclusive UART");
        exclusive = true;
        // Finish setup in a separate method so cleanup runs even if it fails.
    }
    void configure(uint32_t baud)
    {
        const auto speed = baud_speed(baud);
        if (::tcgetattr(fd.value, &original) < 0) throw system_error("read UART settings");
        auto settings = original;
        ::cfmakeraw(&settings);
        settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
        settings.c_cflag |= CS8 | CLOCAL | CREAD;
        settings.c_iflag &= ~(IXON | IXOFF | IXANY);
        settings.c_cc[VMIN] = 0; settings.c_cc[VTIME] = 0;
        if (::cfsetispeed(&settings, speed) < 0 || ::cfsetospeed(&settings, speed) < 0 ||
            ::tcsetattr(fd.value, TCSANOW, &settings) < 0) throw system_error("configure UART");
        configured = true;
    }
    ~SerialPort()
    {
        if (configured) ::tcsetattr(fd.value, TCSANOW, &original);
        if (exclusive) ::ioctl(fd.value, TIOCNXCL);
    }
    bool send(const std::vector<uint8_t>& wire)
    {
        size_t written = 0;
        const auto deadline = monotonic_ms() + SERIAL_WRITE_TIMEOUT_MS;
        while (written < wire.size()) {
            if (!wait_fd(fd.value, POLLOUT, deadline)) return false;
            const auto n = ::write(fd.value, wire.data() + written, wire.size() - written);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) return false;
            written += size_t(n);
        }
        return true;
    }
};

struct SocketEndpoint {
    std::string path;
    FileDescriptor lock;
    FileDescriptor listener;
    bool bound = false;
    explicit SocketEndpoint(const std::string& target)
        : path(socket_path(target)),
          lock(open_lock()),
          listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0))
    {
        if (listener.value < 0) throw system_error("create console socket");
    }
    int open_lock()
    {
        const auto directory = socket_directory();
        if (::mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) throw system_error("create UART runtime directory");
        struct stat info{};
        if (::lstat(directory.c_str(), &info) < 0 || !S_ISDIR(info.st_mode) ||
            info.st_uid != ::getuid() || (info.st_mode & 077) != 0)
            throw std::runtime_error("UART runtime directory must be private and owned by the current user");
        FileDescriptor fd(::open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (fd.value < 0 || ::flock(fd.value, LOCK_EX | LOCK_NB) < 0)
            throw system_error("UART console already owns this target");
        const int result = fd.value; fd.value = -1; return result;
    }
    void bind()
    {
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) throw std::invalid_argument("UART socket path is too long");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        struct stat info{};
        if (::lstat(path.c_str(), &info) == 0) {
            if (!S_ISSOCK(info.st_mode) || info.st_uid != ::getuid())
                throw std::runtime_error("refusing to replace a non-socket UART endpoint");
            if (::unlink(path.c_str()) < 0) throw system_error("remove stale UART socket");
        } else if (errno != ENOENT) throw system_error("inspect UART socket");
        if (::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            throw system_error("bind UART socket");
        bound = true;
        if (::chmod(path.c_str(), 0600) < 0 || ::listen(listener.value, MAX_CLIENTS) < 0)
            throw system_error("listen on UART socket");
    }
    ~SocketEndpoint() { if (bound) ::unlink(path.c_str()); }
};

// Slow terminals/files must not block UART receive or response delivery.
struct LogOutput {
    int flags = ::fcntl(STDOUT_FILENO, F_GETFL);
    bool enabled = false;
    std::string pending;
    uint64_t dropped_bytes = 0;
    LogOutput() { enabled = flags >= 0 && ::fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) == 0; }
    ~LogOutput()
    {
        flush();
        dropped_bytes += pending.size();
        if (flags >= 0) ::fcntl(STDOUT_FILENO, F_SETFL, flags);
        if (dropped_bytes) std::fprintf(stderr, "UART console output dropped %llu bytes (slow/closed output)\n",
                                       static_cast<unsigned long long>(dropped_bytes));
    }
    void append(const std::string& text)
    {
        if (!enabled || text.size() > MAX_OUTPUT - pending.size()) { dropped_bytes += text.size(); return; }
        pending += text;
    }
    void flush()
    {
        if (!enabled || pending.empty()) return;
        const auto n = ::write(STDOUT_FILENO, pending.data(), pending.size());
        if (n > 0) pending.erase(0, size_t(n));
        else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            enabled = false; dropped_bytes += pending.size(); pending.clear();
        }
    }
};
struct Client {
    std::array<uint8_t, RPC_SIZE> input{};
    size_t used = 0;
    std::vector<uint8_t> output;
    size_t sent = 0;
    bool waiting = false;
    uint64_t deadline = 0;
    void reply(const RpcMessage& message)
    {
        const auto bytes = encode_rpc(message);
        output.assign(bytes.begin(), bytes.end()); sent = 0;
    }
};
struct Request { int client; RpcMessage message; };
struct Clients : std::map<int, Client> {
    ~Clients() { for (const auto& entry : *this) ::close(entry.first); }
};
uint64_t new_session()
{
    std::random_device random;
    uint64_t session = (uint64_t(random()) << 32) ^ random() ^ monotonic_ms() ^ uint64_t(getpid());
    return session && session != UART_IPC_SESSION_V1 ? session : UART_IPC_SESSION_V1 + 1;
}
} // namespace

void run_console(const ConsoleConfig& config, const std::atomic_bool& stop)
{
    config.retry.validate();
    if (config.port.empty()) throw std::invalid_argument("UART --port is required");
    SocketEndpoint endpoint(config.target);
    SerialPort serial(config); serial.configure(config.baud);
    endpoint.bind();
    LogOutput logs;
    const auto session = new_session();
    Transaction transaction(session, config.retry);
    StreamDecoder decoder(session, [&](const Frame& frame) {
        if (frame.type == UART_IPC_TYPE_LOG) logs.append(frame.text);
        else transaction.receive(frame);
    }, [&](const std::string& text) { logs.append(text); });
    Clients clients;
    std::deque<Request> queue;
    std::optional<Request> current;
    uint64_t last_rx = monotonic_ms();
    const auto budget = config.retry.client_budget_ms();
    std::fprintf(stderr, "UART console ready: target=%s port=%s socket=%s\n",
                 config.target.c_str(), config.port.c_str(), endpoint.path.c_str());

    while (!stop.load(std::memory_order_relaxed)) {
        std::vector<pollfd> events{{endpoint.listener.value, POLLIN, 0}, {serial.fd.value, POLLIN, 0}};
        if (logs.enabled && !logs.pending.empty()) events.push_back({STDOUT_FILENO, POLLOUT, 0});
        for (const auto& item : clients) events.push_back({item.first,
            short(POLLIN | (item.second.output.empty() ? 0 : POLLOUT)), 0});
        const auto count = ::poll(events.data(), events.size(), 10);
        if (count < 0) { if (errno == EINTR) continue; throw system_error("poll UART console"); }
        const auto now = monotonic_ms();
        if (events[1].revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("UART port disconnected");
        if (events[1].revents & POLLIN) {
            std::array<uint8_t, 4096> bytes{};
            // Bound work per loop even if firmware logs continuously.
            for (int batch = 0; batch < 16; ++batch) {
                const auto n = ::read(serial.fd.value, bytes.data(), bytes.size());
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno != EAGAIN) throw system_error("read UART");
                if (n <= 0) break;
                last_rx = now; decoder.feed(bytes.data(), size_t(n));
            }
        }
        if (now - last_rx >= 1000) { decoder.flush_partial_text(); last_rx = now; }
        logs.flush();

        std::vector<int> closed;
        for (auto& item : clients) {
            const int fd = item.first;
            auto& client = item.second;
            const auto found = std::find_if(events.begin() + 2, events.end(),
                                           [fd](const pollfd& p) { return p.fd == fd; });
            if (found == events.end()) continue;
            bool close = now >= client.deadline || (found->revents & (POLLERR | POLLHUP | POLLNVAL));
            if (!close && (found->revents & POLLOUT) && !client.output.empty()) {
                const auto n = ::send(fd, client.output.data() + client.sent,
                                      client.output.size() - client.sent, MSG_NOSIGNAL);
                if (n > 0) {
                    client.sent += size_t(n);
                    if (client.sent == client.output.size()) {
                        client.output.clear(); client.sent = 0;
                        if (!client.waiting && client.used == 0)
                            client.deadline = std::numeric_limits<uint64_t>::max();
                    }
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) close = true;
            }
            if (!close && (found->revents & POLLIN)) {
                const auto n = ::recv(fd, client.input.data() + client.used, RPC_SIZE - client.used, 0);
                if (n > 0) {
                    if (client.used == 0 && !client.waiting) client.deadline = now + 2000;
                    client.used += size_t(n);
                    if (client.used == RPC_SIZE) {
                        RpcMessage request;
                        if (client.waiting || !client.output.empty() ||
                            !decode_rpc(client.input.data(), request) || request.id == 0 ||
                            (request.kind != RpcKind::Read && request.kind != RpcKind::Write) ||
                            request.result != UartResult::Ok || request.firmware_status != 0 || request.budget_ms != 0) {
                            close = true;
                        } else {
                            client.waiting = true; client.used = 0; client.deadline = now + budget;
                            queue.push_back({fd, request});
                        }
                    }
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) close = true;
            }
            if (close) closed.push_back(fd);
        }
        for (int fd : closed) {
            ::close(fd); clients.erase(fd);
            queue.erase(std::remove_if(queue.begin(), queue.end(), [fd](const Request& r) { return r.client == fd; }), queue.end());
            // Drain an already submitted transaction before serving another
            // client. It cannot be cancelled on the firmware side.
            if (current && current->client == fd) current->client = -1;
        }
        if (events[0].revents & POLLIN) {
            // One accept per loop bounds connection-flood work.
            const int fd = ::accept4(endpoint.listener.value, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                if (clients.size() >= MAX_CLIENTS) ::close(fd);
                else {
                    auto& client = clients[fd]; client.deadline = now + 5000;
                    RpcMessage hello; hello.budget_ms = budget; client.reply(hello);
                }
            } else if (errno != EAGAIN && errno != EINTR) throw system_error("accept UART client");
        }
        // A response read in this loop wins over its deadline/retry.
        transaction.tick(now);
        if (current && !transaction.active()) {
            const auto client = clients.find(current->client);
            if (client != clients.end()) {
                RpcMessage reply; reply.kind = RpcKind::Reply; reply.id = current->message.id;
                reply.result = transaction.result().result;
                reply.firmware_status = transaction.result().firmware_status;
                reply.value = transaction.result().value;
                client->second.reply(reply); client->second.waiting = false;
                client->second.deadline = now + 5000;
            }
            current.reset();
        }
        if (!current && !queue.empty()) {
            current = queue.front(); queue.pop_front();
            transaction.start(current->message.kind == RpcKind::Write, current->message.address,
                              current->message.value, now);
        }
        if (transaction.needs_send()) {
            if (!serial.send(transaction.wire())) throw std::runtime_error("UART write failed; command outcome may be unknown");
            transaction.sent(monotonic_ms());
        }
    }
    if (decoder.bad_frames() || decoder.oversized_frames() || decoder.stale_session_frames())
        std::fprintf(stderr, "UART RX discarded: bad_frames=%u oversized_frames=%u stale_session_frames=%llu\n",
                     decoder.bad_frames(), decoder.oversized_frames(),
                     static_cast<unsigned long long>(decoder.stale_session_frames()));
}
} // namespace uart
#else
namespace uart {
void run_console(const ConsoleConfig&, const std::atomic_bool&)
{
    throw std::runtime_error("UART console requires Linux");
}
} // namespace uart
#endif
