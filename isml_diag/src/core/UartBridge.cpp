#include "diag/core/UartBridge.h"
#include "uart/UartProtocol.h"
#include "uart/UartPosix.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#if defined(__linux__)
#include <cstring>
#include <ctime>
#include <random>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#endif

namespace uart {
std::string find_cp210x_port(const std::string& tty_root, const std::string& dev_root)
{
    namespace fs = std::filesystem;
    std::vector<std::string> ports;
    for (const auto& entry : fs::directory_iterator(tty_root)) {
        std::error_code error;
        auto parent = fs::canonical(entry.path() / "device", error);
        if (error) continue;
        while (!parent.empty()) {
            const auto driver = fs::canonical(parent / "driver", error);
            if (!error && driver.filename() == "cp210x") {
                ports.push_back((fs::path(dev_root) / entry.path().filename()).string());
                break;
            }
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
    }
    std::sort(ports.begin(), ports.end());
    if (ports.size() == 1) return ports.front();
    std::string message = ports.empty() ? "no CP210x UART endpoint found" :
        "multiple CP210x UART endpoints found; tester mapping must identify the PMU port:";
    for (const auto& port : ports) message += " " + port;
    throw std::runtime_error(message);
}
}

#if defined(__linux__)
namespace {
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
    uart::FileDescriptor fd;
    termios original{};
    bool configured = false;
    bool exclusive = false;
    explicit SerialPort(const std::string& port)
        : fd(::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC))
    {
        if (fd.value < 0) throw system_error("open UART " + port);
        if (::flock(fd.value, LOCK_EX | LOCK_NB) < 0) throw system_error("UART already owned");
        if (::ioctl(fd.value, TIOCEXCL) < 0) throw system_error("claim exclusive UART");
        exclusive = true;
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
    ~SerialPort() { close(); }
    void close()
    {
        if (fd.value < 0) return;
        if (configured) ::tcsetattr(fd.value, TCSANOW, &original);
        if (exclusive) ::ioctl(fd.value, TIOCNXCL);
        ::close(fd.value);
        fd.value = -1;
    }
    bool send(const std::vector<uint8_t>& wire)
    {
        size_t written = 0;
        const auto deadline = uart::monotonic_ms() + 1000;
        while (written < wire.size()) {
            if (!uart::wait_fd(fd.value, POLLOUT, deadline)) return false;
            const auto n = ::write(fd.value, wire.data() + written, wire.size() - written);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) return false;
            written += size_t(n);
        }
        return true;
    }
};
uint64_t new_session()
{
    std::random_device random;
    const uint64_t session = (uint64_t(random()) << 32) ^ random() ^
                             uart::monotonic_ms() ^ uint64_t(::getpid());
    return session && session != UART_IPC_SESSION_V1 ? session : UART_IPC_SESSION_V1 + 1;
}

// File writes run separately from UART RX. Bound memory use if the filesystem
// cannot keep up, and report lost bytes/write failures after closing the run.
class LogFile {
    static constexpr size_t MAX_PENDING = 65536;
    uart::FileDescriptor fd_;
    std::string path_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string pending_;
    std::string error_;
    uint64_t dropped_ = 0;
    bool stopping_ = false;
    std::thread worker_;

    void run()
    {
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (pending_.empty() && stopping_) return;
            std::string bytes;
            bytes.swap(pending_);
            lock.unlock();
            size_t done = 0;
            while (done < bytes.size()) {
                const auto count = ::write(fd_.value, bytes.data() + done, bytes.size() - done);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) {
                    const std::string error = system_error("write PMU log " + path_).what();
                    lock.lock();
                    error_ = error;
                    dropped_ += bytes.size() - done + pending_.size();
                    pending_.clear();
                    return;
                }
                done += size_t(count);
            }
        }
    }
public:
    LogFile(bool enabled, uint64_t session)
    {
        if (!enabled) return;
        std::filesystem::create_directories("logs");
        const auto now = std::time(nullptr);
        std::tm local{};
        ::localtime_r(&now, &local);
        std::ostringstream name;
        name << "logs/pmu_uart_" << std::put_time(&local, "%Y%m%d_%H%M%S")
             << '_' << ::getpid() << '_' << std::hex << session << ".log";
        path_ = name.str();
        fd_.value = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd_.value < 0) throw system_error("create PMU log " + path_);
        worker_ = std::thread([this] { run(); });
    }
    ~LogFile() { close(); }
    void append(const std::string& text)
    {
        if (fd_.value < 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_.empty() || text.size() > MAX_PENDING - pending_.size()) {
            dropped_ += text.size();
            return;
        }
        pending_ += text;
        changed_.notify_one();
    }
    void close()
    {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        changed_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    const std::string& path() const { return path_; }
    std::string diagnostics() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = error_;
        if (dropped_) result += " PMU log dropped_bytes=" + std::to_string(dropped_);
        return result;
    }
};
}

class UartBridge::Impl {
public:
    const std::string port;
    SerialPort serial;
    const uint64_t session = new_session();
    LogFile logs;
    uart::Transaction transaction;
    uart::StreamDecoder decoder;
    std::mutex callers;
    mutable std::mutex state;
    std::condition_variable changed;
    bool stopping = false;
    std::string failure;
    std::thread worker;

    explicit Impl(const uart::Config& config)
        : port(config.port.empty() ? uart::find_cp210x_port() : config.port),
          serial(port), logs(config.capture_log, session), transaction(session, config.retry),
          decoder(session, [this](const uart::Frame& frame) {
              if (frame.type == UART_IPC_TYPE_LOG) logs.append(frame.text);
              else transaction.receive(frame);
          }, [this](const std::string& text) { logs.append(text); })
    {
        serial.configure(config.baud);
        worker = std::thread([this] { run(); });
    }
    ~Impl() { close(); }
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(state);
            stopping = true;
            if (transaction.active()) transaction.fail(UartResult::Transport);
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
        logs.close();
        serial.close();
    }
    void run()
    {
        try {
            auto last_rx = uart::monotonic_ms();
            for (;;) {
                pollfd event{serial.fd.value, POLLIN, 0};
                const auto count = ::poll(&event, 1, 10);
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) throw system_error("poll UART");
                std::lock_guard<std::mutex> lock(state);
                if (stopping) break;
                if (event.revents & (POLLERR | POLLHUP | POLLNVAL))
                    throw std::runtime_error("UART port disconnected");
                if (event.revents & POLLIN) {
                    std::array<uint8_t, 4096> bytes{};
                    for (int batch = 0; batch < 16; ++batch) {
                        const auto n = ::read(serial.fd.value, bytes.data(), bytes.size());
                        if (n < 0 && errno == EINTR) continue;
                        if (n < 0 && errno != EAGAIN) throw system_error("read UART");
                        if (n <= 0) break;
                        last_rx = uart::monotonic_ms();
                        decoder.feed(bytes.data(), size_t(n));
                    }
                }
                const auto now = uart::monotonic_ms();
                if (now - last_rx >= 1000) { decoder.flush_partial_text(); last_rx = now; }
                // Read matching replies before checking deadlines. Logs and
                // late replies continue to be consumed during retry backoff.
                transaction.tick(now);
                if (transaction.needs_send()) {
                    if (!serial.send(transaction.wire()))
                        throw std::runtime_error("UART write failed; command outcome may be unknown");
                    transaction.sent(uart::monotonic_ms());
                }
                changed.notify_all();
            }
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(state);
            failure = error.what();
            transaction.fail(UartResult::Transport);
            changed.notify_all();
        }
        std::lock_guard<std::mutex> lock(state);
        decoder.flush_partial_text();
    }
};
#else
class UartBridge::Impl {};
#endif

UartBridge::UartBridge() = default;
UartBridge::~UartBridge() = default;
bool UartBridge::open(const uart::Config& config, std::string* error)
{
    if (error) error->clear();
    if (impl_) { if (error) *error = "UART session is already open"; return false; }
    try {
        config.retry.validate();
#if defined(__linux__)
        impl_ = std::make_unique<Impl>(config);
        return true;
#else
        throw std::runtime_error("UART serial transport requires Linux");
#endif
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}
void UartBridge::close()
{
#if defined(__linux__)
    if (impl_) impl_->close();
#endif
}
UartResponse UartBridge::transact(bool writing, uint64_t address, uint32_t value)
{
    if (address % 4) return {UartResult::Invalid, 0, 0, "UART address must be 32-bit aligned"};
#if defined(__linux__)
    if (impl_) {
        std::lock_guard<std::mutex> caller(impl_->callers);
        std::unique_lock<std::mutex> lock(impl_->state);
        if (impl_->stopping || !impl_->failure.empty())
            return {UartResult::Transport, 0, 0, impl_->failure.empty() ? "UART session closed" : impl_->failure};
        impl_->transaction.start(writing, address, value, uart::monotonic_ms());
        impl_->changed.wait(lock, [&] { return !impl_->transaction.active(); });
        const auto result = impl_->transaction.result();
        std::string error;
        switch (result.result) {
        case UartResult::Timeout: error = "no matching response after configured retries; command outcome may be unknown"; break;
        case UartResult::Firmware: error = "firmware status=" + std::to_string(result.firmware_status); break;
        case UartResult::Transport: error = impl_->failure.empty() ? "UART session closed or request IDs exhausted" : impl_->failure; break;
        case UartResult::Invalid: error = "invalid UART request"; break;
        default: break;
        }
        return {result.result, result.firmware_status, result.value, error};
    }
#else
    (void)writing; (void)value;
#endif
    return {UartResult::Transport, 0, 0, "UART session is not open"};
}
UartResponse UartBridge::read(uint64_t address) { return transact(false, address, 0); }
UartResponse UartBridge::write(uint64_t address, uint32_t value) { return transact(true, address, value); }
std::string UartBridge::port() const
{
#if defined(__linux__)
    if (impl_) return impl_->port;
#endif
    return {};
}
std::string UartBridge::log_path() const
{
#if defined(__linux__)
    if (impl_) return impl_->logs.path();
#endif
    return {};
}
std::string UartBridge::diagnostics() const
{
#if defined(__linux__)
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->state);
        auto result = impl_->failure;
        if (impl_->decoder.bad_frames() || impl_->decoder.oversized_frames() || impl_->decoder.stale_session_frames())
            result += " UART RX discarded: bad_frames=" + std::to_string(impl_->decoder.bad_frames()) +
                " oversized_frames=" + std::to_string(impl_->decoder.oversized_frames()) +
                " stale_session_frames=" + std::to_string(impl_->decoder.stale_session_frames());
        const auto log = impl_->logs.diagnostics();
        if (!log.empty()) result += " " + log;
        return result;
    }
#endif
    return {};
}
