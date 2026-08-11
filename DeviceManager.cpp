#include "DeviceManager.h"

#include <iostream>

DeviceManager::DeviceManager()
    : policy_(load_policy())
{
}

DeviceManager::~DeviceManager()
{
    clear_discovered_devices();
}

std::vector<PolicyEntry> DeviceManager::load_policy() const
{
    // Pseudocode:
    // 1. Read platform/SKU policy from yaml/json.
    // 2. Map physical locators such as BDF/slot/serial to stable target names.
    // 3. Return the names that CLI/Python/reporting should use.
    return {
        {"0000:65:00.0", "TPU_0", "UBB0", 0},
        {"0000:ca:00.0", "TPU_1", "UBB0", 1},
    };
}

std::vector<DeviceContext> DeviceManager::scan_pci_devices() const
{
    // Pseudocode:
    // 1. Walk /sys/bus/pci/devices.
    // 2. Read vendor, device, revision, and BAR resource files.
    // 3. Return observed PCI devices before policy filtering.
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

const PolicyEntry* DeviceManager::find_policy_for_bdf(const std::string& bdf) const
{
    for (const auto& entry : policy_) {
        if (entry.bdf == bdf) {
            return &entry;
        }
    }
    return nullptr;
}

bool DeviceManager::is_supported_tpu(const DeviceContext& ctx) const
{
    return ctx.vendor_id == TPU_VENDOR_ID && ctx.device_id == TPU_DEVICE_ID;
}

DeviceContext DeviceManager::mmap_bar_space(DeviceContext ctx)
{
    // Pseudocode:
    // 1. Open /sys/bus/pci/devices/<bdf>/resource0 or a VFIO region.
    // 2. mmap BAR0 into user space.
    // 3. Keep the mapping lifetime owned by DeviceManager.
    mapped_bar_storage_.push_back(
        std::make_unique<std::vector<uint8_t>>(TPU_BAR_SIZE, 0));

    ctx.mapped_bar_base = mapped_bar_storage_.back()->data();
    ctx.bar_size = TPU_BAR_SIZE;
    return ctx;
}

void DeviceManager::clear_discovered_devices()
{
    target_registry_.clear();
    devices_.clear();
    mapped_bar_storage_.clear();
    device_tree_ = {};
}

void DeviceManager::register_target(BaseDevice* target)
{
    if (target != nullptr) {
        target_registry_[target->get_name()] = target;
    }
}

DeviceDiscoveryInfo DeviceManager::make_tpu_info(const TPUDevice& device,
                                                 const PolicyEntry& policy_entry) const
{
    const auto& ctx = device.get_context();

    DeviceDiscoveryInfo info;
    info.name = device.get_name();
    info.type = "TPU";
    info.parent = "PCIeRootComplex0";
    info.bdf = ctx.bdf;
    info.vendor_id = ctx.vendor_id;
    info.device_id = ctx.device_id;
    info.locator = {
        {"pci_bdf", ctx.bdf},
        {"slot", policy_entry.slot},
    };

    for (const auto* child : device.child_targets()) {
        info.children.push_back(child->get_name());
    }

    return info;
}

DeviceDiscoveryInfo DeviceManager::make_child_info(const BaseDevice& child,
                                                   const TPUDevice& parent,
                                                   const std::string& type) const
{
    const auto& ctx = parent.get_context();

    DeviceDiscoveryInfo info;
    info.name = child.get_name();
    info.type = type;
    info.parent = parent.get_name();
    info.bdf = ctx.bdf;
    info.vendor_id = ctx.vendor_id;
    info.device_id = ctx.device_id;
    info.locator = {
        {"parent", parent.get_name()},
        {"pci_bdf", ctx.bdf},
    };
    return info;
}

void DeviceManager::add_tpu_to_tree(const TPUDevice& device,
                                    const PolicyEntry& policy_entry)
{
    device_tree_.devices.push_back(make_tpu_info(device, policy_entry));
    device_tree_.topology.push_back({"PCIeRootComplex0", device.get_name(), "pcie"});

    if (device.pmu() != nullptr) {
        device_tree_.devices.push_back(make_child_info(*device.pmu(), device, "PMU"));
        device_tree_.topology.push_back({device.get_name(), device.pmu()->get_name(), "module"});
    }

    for (size_t i = 0; i < 2; ++i) {
        auto* isi = device.isi(i);
        if (isi != nullptr) {
            device_tree_.devices.push_back(make_child_info(*isi, device, "ISI"));
            device_tree_.topology.push_back({device.get_name(), isi->get_name(), "module"});
        }
    }

    if (device.ddp() != nullptr) {
        device_tree_.devices.push_back(make_child_info(*device.ddp(), device, "DDP"));
        device_tree_.topology.push_back({device.get_name(), device.ddp()->get_name(), "module"});
    }
}

DeviceTree DeviceManager::discover()
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

        auto mapped_ctx = mmap_bar_space(pci_device);
        auto tpu_device = std::make_unique<TPUDevice>(
            policy_entry->logical_name, mapped_ctx, policy_entry->tpu_index);

        auto* tpu = tpu_device.get();
        register_target(tpu);
        for (auto* child : tpu->child_targets()) {
            register_target(child);
        }

        add_tpu_to_tree(*tpu, *policy_entry);
        devices_.push_back(std::move(tpu_device));
    }

    return device_tree_;
}

BaseDevice* DeviceManager::get_target(const std::string& target_name)
{
    auto it = target_registry_.find(target_name);
    return it == target_registry_.end() ? nullptr : it->second;
}

std::vector<std::string> DeviceManager::get_target_names() const
{
    std::vector<std::string> names;
    for (const auto& device : device_tree_.devices) {
        names.push_back(device.name);
    }
    return names;
}

TestResult DeviceManager::run_atomic_test(const std::string& target_name,
                                          const std::string& test_name,
                                          const TestArgs& args)
{
    auto* target = get_target(target_name);
    if (target == nullptr) {
        return {test_name, target_name, false, {{"error", "target device not found"}}};
    }

    return target->run_atomic_test(test_name, args);
}

void DeviceManager::print_tree() const
{
    for (const auto& device : devices_) {
        device->print_tree();
    }
}
