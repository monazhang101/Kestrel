#pragma once

#include "BaseDevice.h"
#include "TPUDevice.h"

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

struct PolicyEntry {
    std::string bdf;
    std::string logical_name;
    std::string slot;
    uint32_t tpu_index = 0;
};

class DeviceManager {
private:
    static constexpr uint16_t TPU_VENDOR_ID = 0x1d0f;
    static constexpr uint16_t TPU_DEVICE_ID = 0x1000;
    static constexpr uint64_t TPU_BAR_SIZE  = 0x7000;

    std::vector<std::unique_ptr<TPUDevice>> devices_;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> mapped_bar_storage_;
    std::unordered_map<std::string, BaseDevice*> target_registry_;
    std::vector<PolicyEntry> policy_;
    DeviceTree device_tree_;

    std::vector<PolicyEntry> load_policy() const;
    std::vector<DeviceContext> scan_pci_devices() const;
    const PolicyEntry* find_policy_for_bdf(const std::string& bdf) const;
    bool is_supported_tpu(const DeviceContext& ctx) const;
    DeviceContext mmap_bar_space(DeviceContext ctx);
    void clear_discovered_devices();
    void register_target(BaseDevice* target);
    DeviceDiscoveryInfo make_tpu_info(const TPUDevice& device,
                                      const PolicyEntry& policy_entry) const;
    DeviceDiscoveryInfo make_child_info(const BaseDevice& child,
                                        const TPUDevice& parent,
                                        const std::string& type) const;
    void add_tpu_to_tree(const TPUDevice& device,
                         const PolicyEntry& policy_entry);

public:
    DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    ~DeviceManager();

    DeviceTree discover();
    BaseDevice* get_target(const std::string& target_name);
    std::vector<std::string> get_target_names() const;
    TestResult run_atomic_test(const std::string& target_name,
                               const std::string& test_name,
                               const TestArgs& args = {});
    void print_tree() const;
};
