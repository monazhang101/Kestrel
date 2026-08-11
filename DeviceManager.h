#pragma once

#include "TPUDevice.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct DeviceDiscoveryInfo {
    std::string name;
    std::string type;
    std::string bdf;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;
    std::unordered_map<std::string, std::string> locator;
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
};

class DeviceManager {
private:
    std::vector<TPUDevice*> devices_;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> mapped_bar_storage_;
    std::vector<PolicyEntry> policy_;
    DeviceTree device_tree_;

    static constexpr uint16_t TPU_VENDOR_ID = 0x1d0f;
    static constexpr uint16_t TPU_DEVICE_ID = 0x1000;
    static constexpr uint64_t TPU_BAR_SIZE  = 0x6000;

    std::vector<PolicyEntry> load_policy()
    {
        // Pseudocode:
        // 1. read platform/SKU policy yaml/json
        // 2. map physical locator, such as BDF/slot/serial, to canonical device name
        // 3. return stable target devices for CLI/Python/test reports
        return {
            {"0000:65:00.0", "TPU_0", "UBB0"},
            {"0000:ca:00.0", "TPU_1", "UBB0"},
        };
    }

    std::vector<DeviceContext> scan_pci_devices()
    {
        // Pseudocode:
        // 1. walk /sys/bus/pci/devices
        // 2. read vendor/device/revision files
        // 3. collect PCI devices as observed hardware facts
        DeviceContext tpu0;
        tpu0.bdf = "0000:65:00.0";
        tpu0.vendor_id = TPU_VENDOR_ID;
        tpu0.device_id = TPU_DEVICE_ID;

        DeviceContext tpu1;
        tpu1.bdf = "0000:ca:00.0";
        tpu1.vendor_id = TPU_VENDOR_ID;
        tpu1.device_id = TPU_DEVICE_ID;

        DeviceContext unrelated_device;
        unrelated_device.bdf = "0000:17:00.0";
        unrelated_device.vendor_id = 0xffff;
        unrelated_device.device_id = 0xffff;

        return {tpu0, tpu1, unrelated_device};
    }

    const PolicyEntry* find_policy_for_bdf(const std::string& bdf) const
    {
        for (const auto& entry : policy_) {
            if (entry.bdf == bdf) {
                return &entry;
            }
        }
        return nullptr;
    }

    bool is_supported_tpu(const DeviceContext& ctx) const
    {
        return ctx.vendor_id == TPU_VENDOR_ID && ctx.device_id == TPU_DEVICE_ID;
    }

    DeviceContext mmap_bar_space(DeviceContext ctx)
    {
        // Pseudocode:
        // 1. open /sys/bus/pci/devices/<bdf>/resource0 or VFIO region
        // 2. mmap BAR0 into user space
        // 3. keep BAR lifetime in DeviceManager
        mapped_bar_storage_.push_back(
            std::make_unique<std::vector<uint8_t>>(TPU_BAR_SIZE, 0));

        ctx.mapped_bar_base = mapped_bar_storage_.back()->data();
        ctx.bar_size = TPU_BAR_SIZE;

        return ctx;
    }

    void clear_discovered_devices()
    {
        for (auto* device : devices_) {
            delete device;
        }

        devices_.clear();
        mapped_bar_storage_.clear();
        device_tree_ = {};
    }

    DeviceDiscoveryInfo make_device_info(const TPUDevice& device,
                                         const PolicyEntry& policy_entry) const
    {
        const auto& ctx = device.get_context();

        DeviceDiscoveryInfo info;
        info.name = device.get_name();
        info.type = "TPU";
        info.bdf = ctx.bdf;
        info.vendor_id = ctx.vendor_id;
        info.device_id = ctx.device_id;
        info.locator = {
            {"pci_bdf", ctx.bdf},
            {"slot", policy_entry.slot},
        };

        return info;
    }

public:
    DeviceManager()
        : policy_(load_policy())
    {
    }

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    ~DeviceManager()
    {
        clear_discovered_devices();
    }

    DeviceTree discover()
    {
        clear_discovered_devices();

        auto pci_devices = scan_pci_devices();

        for (auto& pci_device : pci_devices) {
            if (!is_supported_tpu(pci_device)) {
                continue;
            }

            const auto* policy_entry = find_policy_for_bdf(pci_device.bdf);
            if (policy_entry == nullptr) {
                continue;
            }

            DeviceContext mapped_ctx = mmap_bar_space(pci_device);
            auto* tpu_device = new TPUDevice(policy_entry->logical_name, mapped_ctx);

            devices_.push_back(tpu_device);
            device_tree_.devices.push_back(make_device_info(*tpu_device, *policy_entry));
            device_tree_.topology.push_back({"PCIeRootComplex0", tpu_device->get_name(), "pcie"});
        }

        return device_tree_;
    }

    TPUDevice* get_device(const std::string& logical_name)
    {
        for (auto* device : devices_) {
            if (device != nullptr && device->get_name() == logical_name) {
                return device;
            }
        }
        return nullptr;
    }

    std::vector<std::string> get_device_names() const
    {
        std::vector<std::string> names;
        for (const auto* device : devices_) {
            if (device != nullptr) {
                names.push_back(device->get_name());
            }
        }
        return names;
    }

    TestResult run_atomic_test(const std::string& logical_name,
                               const std::string& test_name,
                               const TestArgs& args = {})
    {
        auto* device = get_device(logical_name);
        if (device == nullptr) {
            return {test_name, logical_name, false, {{"error", "device not found"}}};
        }

        return device->run_atomic_test(test_name, args);
    }

    void print_tree() const
    {
        for (const auto* device : devices_) {
            if (device != nullptr) {
                device->print_tree();
            }
        }
    }
};
