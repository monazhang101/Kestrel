#include "uart/UartConsole.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
std::atomic_bool stopped{false};
static_assert(std::atomic_bool::is_always_lock_free, "console signals need a lock-free stop flag");
void stop_handler(int) { stopped.store(true, std::memory_order_relaxed); }
uint32_t number(const std::string& text)
{
    size_t used = 0;
    if (text.empty() || text[0] == '-') throw std::invalid_argument("expected an unsigned number");
    const auto value = std::stoull(text, &used, 0);
    if (used != text.size() || value > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("expected a 32-bit unsigned number");
    return uint32_t(value);
}
}
int main(int argc, char** argv)
{
    try {
        uart::ConsoleConfig config;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--help" || flag == "-h") {
                std::cout << "Usage: isml_uart_console --target TPU0 --port <UART endpoint> [options]\n"
                          << "  --baud 115200                8N1, no flow control\n"
                          << "  --retry-count 3              retries after the first send\n"
                          << "  --retry-interval-ms 100      delay after an unanswered attempt\n"
                          << "  --response-timeout-ms 5000   response wait per attempt\n"
                          << "Prints PMU logs; commands come from diag testcases. Ctrl-C exits.\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + flag);
            const std::string value = argv[++i];
            if (flag == "--target") config.target = value;
            else if (flag == "--port") config.port = value;
            else if (flag == "--baud") config.baud = number(value);
            else if (flag == "--retry-count") config.retry.retry_count = number(value);
            else if (flag == "--retry-interval-ms") config.retry.retry_interval_ms = number(value);
            else if (flag == "--response-timeout-ms") config.retry.response_timeout_ms = number(value);
            else throw std::invalid_argument("unknown option " + flag);
        }
        config.retry.validate();
        std::signal(SIGINT, stop_handler); std::signal(SIGTERM, stop_handler);
#if defined(SIGPIPE)
        std::signal(SIGPIPE, SIG_IGN);
#endif
        uart::run_console(config, stopped);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "UART console: " << e.what() << '\n';
        return 1;
    }
}
