#include "diag/device/DeviceManager.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

DeviceManager::DeviceManager(HalType hal_type)
    : hal_(hal_type),
      testcase_policies_(load_testcase_policies("isml_diag/policies/testcases")),
      policy_(load_product_policy({
          "isml_diag/policies/product/atlas_ubb.yaml",
          "isml_diag/policies/product/atlas_m.yaml",
      }))
{
}

DeviceManager::~DeviceManager()
{
    clear_discovered_devices();
}

void DeviceManager::clear_discovered_devices()
{
    target_registry_.clear();
    devices_.clear();
    hal_.clear();
    device_tree_ = {};
}

void DeviceManager::register_device_tree(TestTarget* target)
{
    if (target == nullptr) {
        return;
    }

    // Keep module objects discoverable; testcase dispatch accepts TPU targets.
    target_registry_[target->get_name()] = target;
    for (auto* child : target->child_targets()) {
        register_device_tree(child);
    }
}

void DeviceManager::add_child_to_tree(const TestTarget& child,
                                      const TestTarget& parent)
{
    const auto& ctx = child.get_context();

    DeviceDiscoveryInfo info;
    info.name = child.get_name();
    info.type = child.get_target_type();
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

    device_tree_.devices.push_back(std::move(info));
    device_tree_.topology.push_back({parent.get_name(), child.get_name(), "module"});

    for (const auto* grandchild : child.child_targets()) {
        if (grandchild != nullptr) {
            add_child_to_tree(*grandchild, child);
        }
    }
}

DeviceTree DeviceManager::discover()
{
    std::lock_guard<std::mutex> lock(execution_mutex_);
    // Start a fresh discovery result and release previous transient mappings.
    clear_discovered_devices();

    // Ask the selected HAL context for observed PCI devices.
    auto pci_devices = hal_.scan_pci_devices();

    for (auto& pci_device : pci_devices) {
        // FPGA bring-up intentionally matches chips by VID/DID only because
        // the platform and BDF assignment are not stable yet. Silicon-system
        // discovery will additionally bind VID/DID/BDF to a stable TPU index.
        const PolicyEntry* policy_entry = nullptr;
        for (const auto& entry : policy_) {
            if (entry.match_vendor_id == pci_device.vendor_id &&
                entry.match_device_id == pci_device.device_id) {
                policy_entry = &entry;
                break;
            }
        }
        if (policy_entry == nullptr) {
            continue;
        }

        // Map the matched device BAR through the HAL context.
        auto mapped_ctx = hal_.mmap_bar_space(pci_device);
        mapped_ctx.tpu_type = policy_entry->tpu_type;
        // Bind operation implementations for the matched TPU type.
        auto implementer = std::make_shared<Implementer>(policy_entry->tpu_type);
        mapped_ctx.memory_regions = implementer->memory_regions();

        auto device_config = policy_entry->device_config;
        device_config.tpu_index = static_cast<uint32_t>(devices_.size());
        device_config.name = "TPU" + std::to_string(device_config.tpu_index);

        // Build the TPU object and policy-defined child modules.
        auto tpu_device = std::make_unique<TPUDevice>(
            device_config.name,
            mapped_ctx,
            device_config,
            implementer);

        auto* tpu = tpu_device.get();

        // Register the TPU and child targets for testcase lookup.
        register_device_tree(tpu);

        // Add the discovered TPU and its module hierarchy to the public topology.
        const auto& ctx = tpu->get_context();
        DeviceDiscoveryInfo info;
        info.name = tpu->get_name();
        info.type = tpu->get_target_type();
        info.parent = "PCIeRootComplex0";
        info.bdf = ctx.bdf;
        info.vendor_id = ctx.vendor_id;
        info.device_id = ctx.device_id;
        info.bars = ctx.bar_mappings;
        if (!device_config.slot.empty()) {
            info.locator["slot"] = device_config.slot;
        }
        if (!device_config.position.empty()) {
            info.locator["position"] = device_config.position;
        }
        info.locator["index"] = std::to_string(device_config.tpu_index);

        for (const auto* child : tpu->child_targets()) {
            info.children.push_back(child->get_name());
        }

        device_tree_.devices.push_back(std::move(info));
        device_tree_.topology.push_back({"PCIeRootComplex0", tpu->get_name(), "pcie"});

        for (const auto* child : tpu->child_targets()) {
            if (child != nullptr) {
                add_child_to_tree(*child, *tpu);
            }
        }
        devices_.push_back(std::move(tpu_device));
    }

    return device_tree_;
}

HalType DeviceManager::get_hal_type() const
{
    return hal_.type();
}

TestTarget* DeviceManager::get_target(const std::string& target_name)
{
    auto it = target_registry_.find(target_name);
    return it == target_registry_.end() ? nullptr : it->second;
}

std::vector<std::string> DeviceManager::get_target_names() const
{
    std::vector<std::string> names;
    for (const auto& target : device_tree_.devices) {
        if (target.type == "tpu") names.push_back(target.name);
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

TestStatus DeviceManager::run_testcase(const std::string& target_name,
                                       const std::string& test_name,
                                       const TestArgs& args)
{
    std::lock_guard<std::mutex> lock(execution_mutex_);
    auto* tpu = dynamic_cast<TPUDevice*>(get_target(target_name));
    if (!tpu) {
        logger_.error("unknown TPU target=" + target_name);
        return PHAL_STATUS_INVALID;
    }
    hal_.bind_device(tpu->get_context(), &logger_);
    const auto separator = test_name.find('_');
    auto* target = separator == std::string::npos ? nullptr :
        tpu->module(test_name.substr(0, separator), 0);
    if (!target) {
        logger_.error("test requires an available module prefix: " + test_name);
        return PHAL_STATUS_INVALID;
    }

    const TestcasePolicy* test_policy = nullptr;
    const auto target_policies = testcase_policies_.find(target->get_target_type());
    if (target_policies != testcase_policies_.end()) {
        const auto policy = target_policies->second.find(test_name.substr(separator + 1));
        if (policy != target_policies->second.end()) test_policy = &policy->second;
    }
    return target->run_testcase(test_name, args, &logger_, &hal_, test_policy);
}

void DeviceManager::print_tree() const
{
    for (const auto& device : devices_) {
        device->print_tree();
    }
}
