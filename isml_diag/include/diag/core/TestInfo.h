#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

extern "C" {
#include <phal/phal.h>
}

using TestArgs = std::unordered_map<std::string, std::string>;

class HalContext;

// Testcases and PHAL share one status type and bit layout.
using TestStatus = phal_status_t;
static_assert(PHAL_STATUS_OK == 0 && PHAL_STATUS_TIMEOUT == 1 &&
              PHAL_STATUS_ERROR == 2 && PHAL_STATUS_UNIMPLEMENTED == 4 &&
              PHAL_STATUS_INVALID == 8,
              "PHAL status bits differ from the diagnostic status contract");

constexpr TestStatus operator|(TestStatus lhs, TestStatus rhs)
{
    return static_cast<TestStatus>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

inline TestStatus& operator|=(TestStatus& lhs, TestStatus rhs)
{
    return lhs = lhs | rhs;
}

inline std::string test_status_name(TestStatus status)
{
    if (status == PHAL_STATUS_OK) return "OK";
    std::string result;
    const auto append = [&](const char* name) {
        if (!result.empty()) result += '|';
        result += name;
    };
    const auto bits = static_cast<unsigned>(status);
    if (bits & static_cast<unsigned>(PHAL_STATUS_TIMEOUT)) append("TIMEOUT");
    if (bits & static_cast<unsigned>(PHAL_STATUS_ERROR)) append("ERROR");
    if (bits & static_cast<unsigned>(PHAL_STATUS_UNIMPLEMENTED)) append("UNIMPLEMENTED");
    if (bits & static_cast<unsigned>(PHAL_STATUS_INVALID)) append("INVALID");
    if (bits & ~15u) {
        append("UNKNOWN");
    }
    return result;
}

enum class LogLevel {
    Error = 0,
    Info = 1,
    Debug = 2,
    Trace = 3,
};

class Logger {
private:
    LogLevel level_ = LogLevel::Info;

    bool should_log(LogLevel message_level) const
    {
        return static_cast<int>(message_level) <= static_cast<int>(level_);
    }

    void write(LogLevel message_level,
               const std::string& level_name,
               const std::string& message) const
    {
        if (!should_log(message_level)) {
            return;
        }

        std::cout << "[" << level_name << "] " << message << std::endl;
    }

public:
    void set_level(LogLevel level) { level_ = level; }
    LogLevel get_level() const { return level_; }

    void error(const std::string& message) const { write(LogLevel::Error, "ERROR", message); }
    void info(const std::string& message) const { write(LogLevel::Info, "INFO", message); }
    void debug(const std::string& message) const { write(LogLevel::Debug, "DEBUG", message); }
    void trace(const std::string& message) const { write(LogLevel::Trace, "TRACE", message); }
};

struct TestInfo {
    std::string target_name;
    std::string test_name;
    TestArgs args;
    Logger* logger = nullptr;
    HalContext* hal = nullptr;
};

inline TestStatus make_unimplemented_status(const TestInfo& ti,
                                             const std::string& reason)
{
    if (ti.logger != nullptr) {
        ti.logger->error(reason);
    }
    return PHAL_STATUS_UNIMPLEMENTED;
}
