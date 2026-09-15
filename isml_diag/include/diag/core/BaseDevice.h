#pragma once

#include "diag/core/PlatformPolicy.h"
#include "diag/core/TestInfo.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
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

class BaseDevice {
private:
    std::mutex atomic_test_mutex_;

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

    virtual TestStatus run_atomic_test(const std::string& test_name,
                                       const TestArgs& args = {},
                                       Logger* logger = nullptr,
                                       HalContext* hal = nullptr)
    {
        std::lock_guard<std::mutex> lock(atomic_test_mutex_);
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
            ti.logger->error("atomic test is not registered: target=" + name_ +
                             " test=" + test_name);
            return TestStatus::UNIMPLEMENTED;
        }

        TestStatus status = TestStatus::ERROR;
        try {
            status = it->second(ti);
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
