#pragma once

#include "diag/core/TestTarget.h"
#include "diag/core/HalContext.h"
#include "diag/device/TPUDevice.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct DeviceDiscoveryInfo {
    std::string name;
    std::string type;
    std::string parent;
    std::string bdf;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;
    std::unordered_map<std::string, std::string> locator;
    std::vector<BarMapping> bars;
    std::vector<std::string> children;
};

struct TopologyEdge {
    std::string parent;
    std::string child;
    std::string type;
};

struct DeviceTree {
    std::vector<DeviceDiscoveryInfo> devices;
    std::vector<TopologyEdge> topology;
};

class DeviceManager {
private:
    // Discovery must not invalidate mappings while a testcase is running.
    std::mutex execution_mutex_;
    HalContext hal_;
    std::vector<std::unique_ptr<TPUDevice>> devices_;
    std::unordered_map<std::string, TestTarget*> target_registry_;
    TestcasePolicyCatalog testcase_policies_;
    std::vector<PolicyEntry> policy_;
    DeviceTree device_tree_;
    Logger logger_;

    void clear_discovered_devices();
    void register_device_tree(TestTarget* target);
    void add_child_to_tree(const TestTarget& child,
                           const TestTarget& parent);

public:
    explicit DeviceManager(HalType hal_type = HalType::iHal);

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    ~DeviceManager();

    HalType get_hal_type() const;
    DeviceTree discover();
    TestTarget* get_target(const std::string& target_name);
    std::vector<std::string> get_target_names() const;
    void set_log_level(LogLevel level);
    LogLevel get_log_level() const;
    TestStatus run_testcase(const std::string& target_name,
                            const std::string& test_name,
                            const TestArgs& args = {});
    void print_tree() const;
};
