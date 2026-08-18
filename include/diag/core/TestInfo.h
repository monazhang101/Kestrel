#pragma once

#include <iostream>
#include <string>
#include <unordered_map>

using TestArgs = std::unordered_map<std::string, std::string>;
using TestMetrics = std::unordered_map<std::string, std::string>;

class HalSession;

enum class LogLevel {
    Error = 0,
    Info = 1,
    Debug = 2,
    Trace = 3,
};

class Logger {
private:
    LogLevel level_ = LogLevel::Info;
    std::string log_path_;

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

        // Pseudocode: real code should also append this line to log_path_.
        std::cout << "[" << level_name << "] " << message << std::endl;
    }

public:
    void set_level(LogLevel level) { level_ = level; }
    LogLevel get_level() const { return level_; }

    void set_log_path(const std::string& log_path) { log_path_ = log_path; }
    const std::string& get_log_path() const { return log_path_; }

    void error(const std::string& message) const { write(LogLevel::Error, "ERROR", message); }
    void info(const std::string& message) const { write(LogLevel::Info, "INFO", message); }
    void debug(const std::string& message) const { write(LogLevel::Debug, "DEBUG", message); }
    void trace(const std::string& message) const { write(LogLevel::Trace, "TRACE", message); }
};

struct TestInfo {
    std::string run_id;
    std::string cmd_id;
    std::string target_name;
    std::string test_name;
    TestArgs args;
    std::string start_time;
    std::string end_time;
    std::string log_path;
    Logger* logger = nullptr;
    HalSession* hal = nullptr;
};

struct TestResult {
    std::string test_name;
    std::string target_name;
    bool passed = false;
    TestMetrics metrics;
    std::string error_description;
    std::string error_details;
};
