#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

using TestArgs = std::unordered_map<std::string, std::string>;

class HalContext;

// Keep the public testcase contract intentionally small and PHAL-like: one
// integer status is returned, while diagnostic details go to the testcase log.
enum class TestStatus : int {
    OK = 0,
    TIMEOUT = 1u << 0,
    ERROR = 1u << 1,
    UNIMPLEMENTED = 1u << 2,
    INVALID = 1u << 3,
};

inline const char* test_status_name(TestStatus status)
{
    switch (status) {
    case TestStatus::OK:
        return "OK";
    case TestStatus::TIMEOUT:
        return "TIMEOUT";
    case TestStatus::ERROR:
        return "ERROR";
    case TestStatus::UNIMPLEMENTED:
        return "UNIMPLEMENTED";
    case TestStatus::INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
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
    return TestStatus::UNIMPLEMENTED;
}
