#pragma once

#include "diag/core/TestInfo.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct DeviceContext {
    std::string bdf;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;

    void* mapped_bar_base = nullptr;
    uint64_t bar_size = 0;
};

using AtomicTestFunc = std::function<TestResult(TestInfo& ti)>;

class BaseDevice {
private:
    std::mutex device_mutex_;

protected:
    std::string name_;
    DeviceContext ctx_;
    std::unordered_map<std::string, AtomicTestFunc> registered_tests_;

    void _add_test(const std::string& test_name, AtomicTestFunc func)
    {
        registered_tests_[test_name] = std::move(func);
    }

public:
    BaseDevice(const std::string& name, const DeviceContext& ctx)
        : name_(name), ctx_(ctx)
    {
    }

    virtual ~BaseDevice() = default;

    virtual TestResult run_atomic_test(const std::string& test_name,
                                       const TestArgs& args = {},
                                       Logger* logger = nullptr,
                                       HalSession* hal = nullptr)
    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        Logger default_logger;

        TestInfo ti;
        ti.run_id = "run_id_pseudocode";
        ti.cmd_id = "cmd_id_assigned_when_hardware_command_is_submitted";
        ti.target_name = name_;
        ti.test_name = test_name;
        ti.args = args;
        ti.start_time = "start_time_pseudocode";
        ti.log_path = "./logs/" + name_ + "/" + test_name + ".log";
        ti.logger = logger == nullptr ? &default_logger : logger;
        ti.hal = hal;
        ti.logger->set_log_path(ti.log_path);

        auto it = registered_tests_.find(test_name);
        if (it == registered_tests_.end()) {
            ti.end_time = "end_time_pseudocode";
            return {
                test_name,
                name_,
                false,
                {},
                "atomic test not found",
                "target=" + name_ + ", test=" + test_name
            };
        }

        TestResult result;
        try {
            result = it->second(ti);
        } catch (const std::exception& e) {
            result = {
                test_name,
                name_,
                false,
                {},
                "atomic test failed before execution completed",
                e.what()
            };
        }
        ti.end_time = "end_time_pseudocode";
        return result;
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
