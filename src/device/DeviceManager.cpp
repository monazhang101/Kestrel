#include "diag/device/DeviceManager.h"

#include <memory>
#include <utility>

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

DeviceManager::DeviceManager(HalType hal_type)
    : hal_(hal_type),
      policy_(load_product_policy({
          "policies/product/atlas_ubb.yaml",
          "policies/product/atlas_m.yaml",
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

void DeviceManager::register_device_tree(BaseDevice* target)
{
    if (target == nullptr) {
        return;
    }

    // Register this target and all descendants for run_atomic_test lookup.
    target_registry_[target->get_name()] = target;
    for (auto* child : target->child_targets()) {
        register_device_tree(child);
    }
}

void DeviceManager::add_child_to_tree(const BaseDevice& child,
                                      const BaseDevice& parent)
{
    const auto& ctx = child.get_context();

    DeviceDiscoveryInfo info;
    info.name = child.get_name();
    info.type = infer_child_type(child.get_name());
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
    // Start a fresh discovery result and release previous transient mappings.
    clear_discovered_devices();

    // Ask the selected HAL backend for observed PCI devices.
    auto pci_devices = hal_.scan_pci_devices();

    for (auto& pci_device : pci_devices) {
        // Match observed BDF/VID/DID against all loaded product policies.
        const PolicyEntry* policy_entry = nullptr;
        for (const auto& entry : policy_) {
            if (entry.match_bdf == pci_device.bdf &&
                entry.match_vendor_id == pci_device.vendor_id &&
                entry.match_device_id == pci_device.device_id) {
                policy_entry = &entry;
                break;
            }
        }
        if (policy_entry == nullptr) {
            continue;
        }

        // Map the matched device BAR through the HAL session.
        auto mapped_ctx = hal_.mmap_bar_space(pci_device);

        // Bind operation implementations for the matched TPU type.
        auto implementer = std::make_shared<Implementer>(policy_entry->tpu_type);

        // Build the TPU object and policy-defined child modules.
        auto tpu_device = std::make_unique<TPUDevice>(
            policy_entry->device_config.name,
            mapped_ctx,
            policy_entry->device_config,
            implementer);

        auto* tpu = tpu_device.get();

        // Register the TPU and child targets for run_atomic_test lookup.
        register_device_tree(tpu);

        // Add the discovered TPU and its module hierarchy to the public topology.
        const auto& ctx = tpu->get_context();
        DeviceDiscoveryInfo info;
        info.name = tpu->get_name();
        info.type = "TPU";
        info.parent = "PCIeRootComplex0";
        info.bdf = ctx.bdf;
        info.vendor_id = ctx.vendor_id;
        info.device_id = ctx.device_id;
        info.locator = {
            {"slot", policy_entry->device_config.slot},
            {"position", policy_entry->device_config.position},
            {"index", std::to_string(policy_entry->device_config.tpu_index)},
        };

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

    // -------------------------------
    // Future per-device execution lock boundary.
    //
    // If MVP policy chooses "one test at a time per TPU", derive the top-level
    // TPU name from target_name before dispatch:
    //   ATLAS_0           -> ATLAS_0
    //   ATLAS_0.PCIE_0    -> ATLAS_0
    //   ATLAS_0.DDP_0     -> ATLAS_0
    //   ATLAS_0.DDP_0.DMC_0_0 -> ATLAS_0
    //
    // Then lock device_execution_mutexes_[ATLAS_0] here, before calling
    // target->run_atomic_test(). BaseDevice still keeps its own object mutex,
    // so the final order is:
    //   DeviceManager per-TPU lock -> BaseDevice per-object lock -> test body.
    //
    // This intentionally serializes all modules under one TPU while allowing
    // different TPU devices, such as ATLAS_0 and ATLAS_1, to run in parallel.
    // -------------------------------
    // TODO: Resolve YAML policy defaults/ranges for this target/test before dispatch.
    return target->run_atomic_test(test_name, args, &logger_, &hal_);
}

void DeviceManager::print_tree() const
{
    for (const auto& device : devices_) {
        device->print_tree();
    }
}
