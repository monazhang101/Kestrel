#pragma once

#include "UartProtocol.h"
#include <atomic>
#include <string>

namespace uart {
struct ConsoleConfig {
    std::string target;
    std::string port;
    uint32_t baud = 115200;
    RetryConfig retry;
};

// Foreground service. No PHAL/PCI discovery dependencies and no stdin commands.
// Throws on setup failure or serial disconnect; all clients then disconnect.
void run_console(const ConsoleConfig& config, const std::atomic_bool& stop);
} // namespace uart
