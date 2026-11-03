#pragma once

#include <cstdint>
#include <mutex>
#include <string>

// Host transport status is separate from the firmware's UART_IPC_ERR_* ABI.
enum class UartResult : uint32_t {
    Ok = 0, Timeout = 1, Invalid = 2, Transport = 3, Firmware = 4, Busy = 5,
};

class UartBridge {
public:
    UartBridge() = default;
    ~UartBridge();
    UartBridge(const UartBridge&) = delete;
    UartBridge& operator=(const UartBridge&) = delete;

    bool bind(const std::string& target, std::string* error = nullptr);
    UartResult read(uint64_t address, uint32_t& value);
    UartResult write(uint64_t address, uint32_t value);
    uint32_t firmware_status() const { return firmware_status_; }
    const std::string& error() const { return error_; }

private:
    UartResult transact(bool write, uint64_t address, uint32_t& value);
    void disconnect();
    std::mutex mutex_;
    int fd_ = -1;
    uint32_t budget_ms_ = 0;
    uint32_t next_id_ = 0;
    uint32_t firmware_status_ = 0;
    std::string error_;
};
