#pragma once

#include "diag/core/PlatformPolicy.h"
#include "diag/core/TestInfo.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct BarMapping {
    uint32_t bar_index = 0;
    std::string name;
    void* mapped_base = nullptr;
    uint64_t device_base = 0;
    uint64_t size = 0;
    uint64_t expected_size = 0;
    uint64_t resource_size = 0;
    uint64_t mapped_size = 0;
    bool mapped = false;
    std::string error;
    std::vector<std::string> layout;
};

struct DeviceContext {
    std::string bdf;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;
    TPUType tpu_type = TPUType::Unknown;

    void* mapped_bar_base = nullptr;
    uint64_t bar_device_base = 0;
    uint64_t bar_size = 0;
    std::vector<BarMapping> bar_mappings;

    uint32_t pcie_control_bar_index = 0;
    uint64_t pcie_control_base = 0;
};

using AtomicTestFunc = std::function<TestStatus(TestInfo& ti)>;

struct TestArgumentDefinition {
    std::string name;
    std::string default_value;
    std::string format;
};

struct AtomicTestDefinition {
    std::vector<TestArgumentDefinition> arguments;
    AtomicTestFunc execute;
};

class BaseDevice {
private:
    std::mutex atomic_test_mutex_;

protected:
    std::string name_;
    DeviceContext ctx_;
    std::unordered_map<std::string, AtomicTestDefinition> registered_tests_;

    void _add_test(const std::string& test_name,
                   std::initializer_list<TestArgumentDefinition> arguments,
                   AtomicTestFunc func)
    {
        registered_tests_[test_name] = {
            std::vector<TestArgumentDefinition>(arguments),
            std::move(func),
        };
    }

    static bool parse_u64(const std::string& value, uint64_t& parsed)
    {
        if (value.empty() || value.front() == '-') {
            return false;
        }
        try {
            size_t consumed = 0;
            parsed = std::stoull(value, &consumed, 0);
            return consumed == value.size();
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool is_numeric_format(const std::string& format)
    {
        return format == "u64" || format == "bytes" ||
               format == "ms" || format == "offset";
    }

    static bool validate_format(const TestArgumentDefinition& definition,
                                const std::string& value,
                                std::string& error)
    {
        if (definition.format == "string") {
            return true;
        }
        if (is_numeric_format(definition.format)) {
            uint64_t parsed = 0;
            if (parse_u64(value, parsed)) {
                return true;
            }
            error = "argument " + definition.name + " must use format " +
                    definition.format + ": " + value;
            return false;
        }
        error = "argument " + definition.name + " has unsupported format: " +
                definition.format;
        return false;
    }

    static bool validate_range(const TestArgumentDefinition& definition,
                               const std::string& value,
                               const TestArgumentPolicy& policy,
                               std::string& error)
    {
        if (policy.range.empty()) {
            return true;
        }

        if (is_numeric_format(definition.format) && policy.range.size() == 2) {
            uint64_t parsed = 0;
            uint64_t minimum = 0;
            uint64_t maximum = 0;
            if (!parse_u64(value, parsed) ||
                !parse_u64(policy.range[0], minimum) ||
                !parse_u64(policy.range[1], maximum) ||
                minimum > maximum) {
                error = "invalid numeric policy range for argument: " + definition.name;
                return false;
            }
            if (parsed >= minimum && parsed <= maximum) {
                return true;
            }
        } else {
            for (const auto& allowed : policy.range) {
                if (value == allowed) {
                    return true;
                }
            }
        }

        std::ostringstream message;
        message << "argument " << definition.name << "=" << value
                << " is outside policy range [";
        for (size_t i = 0; i < policy.range.size(); ++i) {
            if (i != 0) {
                message << ", ";
            }
            message << policy.range[i];
        }
        message << "]";
        error = message.str();
        return false;
    }

public:
    BaseDevice(const std::string& name, const DeviceContext& ctx)
        : name_(name), ctx_(ctx)
    {
    }

    virtual ~BaseDevice() = default;

    virtual TestStatus run_atomic_test(const std::string& test_name,
                                       const TestArgs& args = {},
                                       Logger* logger = nullptr,
                                       HalContext* hal = nullptr,
                                       const AtomicTestPolicy* policy = nullptr)
    {
        std::lock_guard<std::mutex> lock(atomic_test_mutex_);
        Logger default_logger;

        TestInfo ti;
        ti.run_id = "run_id_pseudocode";
        ti.cmd_id = "cmd_id_assigned_when_hardware_command_is_submitted";
        ti.target_name = name_;
        ti.test_name = test_name;
        ti.start_time = "start_time_pseudocode";
        ti.log_path = "./logs/" + name_ + "/" + test_name + ".log";
        ti.logger = logger == nullptr ? &default_logger : logger;
        ti.hal = hal;
        ti.logger->set_log_path(ti.log_path);

        auto it = registered_tests_.find(test_name);
        if (it == registered_tests_.end()) {
            ti.end_time = "end_time_pseudocode";
            ti.logger->error("atomic test is not registered: target=" + name_ +
                             " test=" + test_name);
            return TestStatus::UNIMPLEMENTED;
        }

        TestArgs resolved_args;
        std::unordered_map<std::string, const TestArgumentDefinition*> definitions;
        for (const auto& definition : it->second.arguments) {
            resolved_args[definition.name] = definition.default_value;
            definitions[definition.name] = &definition;
        }
        for (const auto& argument : args) {
            if (definitions.find(argument.first) == definitions.end()) {
                ti.logger->error("unknown argument: test=" + test_name +
                                 " argument=" + argument.first);
                return TestStatus::INVALID;
            }
            resolved_args[argument.first] = argument.second;
        }
        for (const auto& definition : it->second.arguments) {
            const auto& value = resolved_args.at(definition.name);
            std::string error;
            if (!validate_format(definition, value, error)) {
                ti.logger->error(error);
                return TestStatus::INVALID;
            }
            if (policy != nullptr) {
                auto policy_argument = policy->arguments.find(definition.name);
                if (policy_argument != policy->arguments.end() &&
                    !validate_range(definition, value, policy_argument->second, error)) {
                    ti.logger->error(error);
                    return TestStatus::INVALID;
                }
            }
        }
        ti.args = std::move(resolved_args);

        TestStatus status = TestStatus::ERROR;
        try {
            status = it->second.execute(ti);
        } catch (const std::invalid_argument& e) {
            ti.logger->error(std::string("invalid test argument: ") + e.what());
            status = TestStatus::INVALID;
        } catch (const std::out_of_range& e) {
            ti.logger->error(std::string("test argument is out of range: ") + e.what());
            status = TestStatus::INVALID;
        } catch (const std::exception& e) {
            ti.logger->error(std::string("atomic test exception: ") + e.what());
            status = TestStatus::ERROR;
        }
        ti.end_time = "end_time_pseudocode";
        return status;
    }

    const std::string& get_name() const { return name_; }

    void* get_bar_base_addr() const { return ctx_.mapped_bar_base; }

    DeviceContext& get_context() { return ctx_; }

    const DeviceContext& get_context() const { return ctx_; }

    std::vector<std::string> get_registered_test_names() const
    {
        std::vector<std::string> names;
        for (const auto& item : registered_tests_) {
            names.push_back(item.first);
        }
        return names;
    }

    virtual std::vector<BaseDevice*> child_targets() const
    {
        return {};
    }
};
