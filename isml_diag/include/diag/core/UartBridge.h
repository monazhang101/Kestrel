#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Host transport status is separate from the firmware's UART_IPC_ERR_* ABI.
enum class UartResult : uint32_t {
    Ok = 0, Timeout = 1, Invalid = 2, Transport = 3, Firmware = 4,
};

namespace uart {
struct RetryConfig {
    uint32_t retry_count = 3; // Retries after the initial send.
    uint32_t retry_interval_ms = 100;
    uint32_t response_timeout_ms = 5000;
    void validate() const;
};

struct Config {
    uint32_t baud = 115200;
    RetryConfig retry;
    bool capture_log = false;
    // The CLI leaves this empty and auto-selects the unique CP210x endpoint.
    // Internal callers/tests can supply an endpoint, such as a PTY.
    std::string port;
};

std::string find_cp210x_port(const std::string& tty_root = "/sys/class/tty",
                            const std::string& dev_root = "/dev");
}

struct UartResponse {
    UartResult result = UartResult::Ok;
    uint32_t firmware_status = 0;
    uint32_t value = 0;
    std::string error;
};

// Owns the serial port and receive worker for a whole run. One transaction
// lock serializes all callers, including response waits and retry intervals.
class UartBridge {
public:
    UartBridge();
    ~UartBridge();
    UartBridge(const UartBridge&) = delete;
    UartBridge& operator=(const UartBridge&) = delete;

    bool open(const uart::Config& config, std::string* error = nullptr);
    void close();
    UartResponse read(uint64_t address);
    UartResponse write(uint64_t address, uint32_t value);
    std::string port() const;
    std::string log_path() const;
    std::string diagnostics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    UartResponse transact(bool writing, uint64_t address, uint32_t value);
};
