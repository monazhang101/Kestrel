#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"
#include "diag/device/TPUDevice.h"
#include "diag/core/HalBackend.h"

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
    HalSession hal_;
    std::vector<std::unique_ptr<TPUDevice>> devices_;
    std::unordered_map<std::string, BaseDevice*> target_registry_;
    // -------------------------------
    // Future per-device execution locks.
    //
    // Use one lock per top-level TPU target, e.g. ATLAS_0 or ATLAS_1, when
    // the MVP needs every module under the same TPU to run sequentially.
    // Child targets such as ATLAS_0.PCIE_0, ATLAS_0.PMU_0, ATLAS_0.DDP_0,
    // and ATLAS_0.ISI_0 should all resolve to the ATLAS_0 lock before
    // dispatching into BaseDevice::run_atomic_test().
    //
    // Example shape:
    // std::unordered_map<std::string, std::mutex> device_execution_mutexes_;
    // -------------------------------
    std::vector<PolicyEntry> policy_;
    DeviceTree device_tree_;
    Logger logger_;

    void clear_discovered_devices();
    void register_device_tree(BaseDevice* target);
    void add_child_to_tree(const BaseDevice& child,
                           const BaseDevice& parent);

public:
    explicit DeviceManager(HalType hal_type = HalType::iHal);

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    ~DeviceManager();

    HalType get_hal_type() const;
    DeviceTree discover();
    BaseDevice* get_target(const std::string& target_name);
    std::vector<std::string> get_target_names() const;
    void set_log_level(LogLevel level);
    LogLevel get_log_level() const;
    TestResult run_atomic_test(const std::string& target_name,
                               const std::string& test_name,
                               const TestArgs& args = {});
    void print_tree() const;
};
