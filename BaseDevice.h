#pragma once

#include <cstdint>
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

using TestArgs = std::unordered_map<std::string, std::string>;

struct TestResult {
    std::string test_name;
    std::string device_name;
    bool passed = false;
    std::unordered_map<std::string, std::string> metrics;
};

using AtomicTestFunc = std::function<TestResult(const TestArgs& args)>;

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
                                       const TestArgs& args = {})
    {
        std::lock_guard<std::mutex> lock(device_mutex_);

        auto it = registered_tests_.find(test_name);
        if (it == registered_tests_.end()) {
            return {test_name, name_, false, {{"error", "atomic test not found"}}};
        }

        return it->second(args);
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
};
