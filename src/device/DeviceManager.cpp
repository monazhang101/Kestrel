#include "diag/device/DeviceManager.h"

#include <iostream>

namespace {

std::string infer_child_type(const std::string& target_name)
{
    if (target_name.find(".PCIE_") != std::string::npos) {
        return "PCIE_MODULE";
    }
    if (target_name.find(".PMU_") != std::string::npos) {
        return "PMU_MODULE";
    }
    if (target_name.find(".DMC_") != std::string::npos) {
        return "DMC_MODULE";
    }
    if (target_name.find(".DDP_") != std::string::npos) {
        return "DDP_MODULE";
    }
    if (target_name.find(".ISI_") != std::string::npos) {
        return "ISI_MODULE";
    }
    return "MODULE";
}

}

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
        {"0000:65:00.0", "ATLAS_0", "UBB0", "UBB0_POS0", 0},
        {"0000:ca:00.0", "ATLAS_1", "UBB0", "UBB0_POS1", 1},
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

void DeviceManager::register_device_tree(BaseDevice* target)
{
    if (target == nullptr) {
        return;
    }

    register_target(target);
    for (auto* child : target->child_targets()) {
        register_device_tree(child);
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
        {"slot", policy_entry.slot},
        {"position", policy_entry.position},
        {"atlas_index", std::to_string(policy_entry.tpu_index)},
    };

    for (const auto* child : device.child_targets()) {
        info.children.push_back(child->get_name());
    }

    return info;
}

DeviceDiscoveryInfo DeviceManager::make_child_info(const BaseDevice& child,
                                                   const BaseDevice& parent,
                                                   const std::string& type) const
{
    const auto& ctx = child.get_context();

    DeviceDiscoveryInfo info;
    info.name = child.get_name();
    info.type = type;
    info.parent = parent.get_name();
    info.bdf = ctx.bdf;
    info.vendor_id = ctx.vendor_id;
    info.device_id = ctx.device_id;
    info.locator = {
        {"parent", parent.get_name()},
    };

    for (const auto* grandchild : child.child_targets()) {
        if (grandchild != nullptr) {
            info.children.push_back(grandchild->get_name());
        }
    }

    return info;
}

void DeviceManager::add_child_to_tree(const BaseDevice& child,
                                      const BaseDevice& parent)
{
    auto type = infer_child_type(child.get_name());
    device_tree_.devices.push_back(make_child_info(child, parent, type));
    device_tree_.topology.push_back({parent.get_name(), child.get_name(), "module"});

    for (const auto* grandchild : child.child_targets()) {
        if (grandchild != nullptr) {
            add_child_to_tree(*grandchild, child);
        }
    }
}

void DeviceManager::add_tpu_to_tree(const TPUDevice& device,
                                    const PolicyEntry& policy_entry)
{
    device_tree_.devices.push_back(make_tpu_info(device, policy_entry));
    device_tree_.topology.push_back({"PCIeRootComplex0", device.get_name(), "pcie"});

    for (const auto* child : device.child_targets()) {
        if (child != nullptr) {
            add_child_to_tree(*child, device);
        }
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
        register_device_tree(tpu);

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
    for (const auto& target : device_tree_.devices) {
        names.push_back(target.name);
    }
    return names;
}

void DeviceManager::set_log_level(LogLevel level)
{
    logger_.set_level(level);
}

LogLevel DeviceManager::get_log_level() const
{
    return logger_.get_level();
}

TestResult DeviceManager::run_atomic_test(const std::string& target_name,
                                          const std::string& test_name,
                                          const TestArgs& args)
{
    auto* target = get_target(target_name);
    if (target == nullptr) {
        return {
            test_name,
            target_name,
            false,
            {},
            "target device not found",
            "target=" + target_name + " is not registered; call discover() before running tests"
        };
    }

    return target->run_atomic_test(test_name, args, &logger_);
}

void DeviceManager::print_tree() const
{
    for (const auto& device : devices_) {
        device->print_tree();
    }
}
