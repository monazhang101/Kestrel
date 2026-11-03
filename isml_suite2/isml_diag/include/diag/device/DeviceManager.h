#pragma once

#include "diag/core/TestTarget.h"
#include "diag/core/HalContext.h"
#include "diag/device/TPUDevice.h"

#include <cstdint>
#include <map>
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
    std::map<std::string, std::string> properties;
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
    void add_inventory_node(const TPUDevice& tpu,
                            const std::string& parent,
                            const std::string& name,
                            const std::string& type,
                            std::map<std::string, std::string> properties = {});
    void add_atlas_inventory_tree(const TPUDevice& tpu);
    TestStatus wait_system_ready(uint32_t timeout_ms = 600000,
                                 uint32_t poll_interval_ms = 1000);
    TestStatus read_atlas_resource_info(AtlasResourceInfo& info);

public:
    explicit DeviceManager(
        const std::string& policy_dir = "isml_diag/policies");

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    ~DeviceManager();

    DeviceTree discover();
    TestTarget* get_target(const std::string& target_name);
    std::vector<std::string> get_target_names() const;
    void set_log_level(LogLevel level);
    LogLevel get_log_level() const;
    TestStatus run_testcase(const std::string& target_name,
                            const std::string& test_name,
                            const TestArgs& args = {});
    std::vector<TestArgs> get_testcase_runs(const std::string& test_name) const;
    void print_tree() const;
};
