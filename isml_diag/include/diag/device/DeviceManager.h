#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/HalContext.h"
#include "diag/core/PlatformPolicy.h"
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
    HalContext hal_;
    std::vector<std::unique_ptr<TPUDevice>> devices_;
    std::unordered_map<std::string, BaseDevice*> target_registry_;
    // -------------------------------
    // Future per-device execution locks.
    //
    // Use one lock per top-level TPU target, e.g. ATLAS_0 or ATLAS_1, when
    // the MVP needs every module under the same TPU to run sequentially.
    // Child targets such as PCIE_0_0, PMU_0_0, DDP_0_1, DMC_0_1_2,
    // and ISI_0_7 should all resolve to the ATLAS_0 lock before
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
    DevMem open_devmem(const DeviceContext& ctx,
                       DevMemSpec spec,
                       Logger* logger = nullptr);
    std::vector<DevMem> open_multi_devmem(const DeviceContext& ctx,
                                          std::vector<DevMemSpec> specs,
                                          Logger* logger = nullptr);
    TestResult run_atomic_test(const std::string& target_name,
                               const std::string& test_name,
                               const TestArgs& args = {});
    void print_tree() const;
};
