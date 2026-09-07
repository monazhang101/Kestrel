#include "diag/device/DeviceManager.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

std::string infer_child_type(const std::string& target_name)
{
    if (target_name.rfind("PCIE_", 0) == 0) {
        return "PCIE_MODULE";
    }
    if (target_name.rfind("PMU_", 0) == 0) {
        return "PMU_MODULE";
    }
    if (target_name.rfind("DMC_", 0) == 0) {
        return "DMC_MODULE";
    }
    if (target_name.rfind("DDP_", 0) == 0) {
        return "DDP_MODULE";
    }
    if (target_name.rfind("ISI_", 0) == 0) {
        return "ISI_MODULE";
    }
    return "MODULE";
}

std::string default_tpu_name(TPUType tpu_type, uint32_t index)
{
    switch (tpu_type) {
    case TPUType::Atlas:
        return "ATLAS_" + std::to_string(index);
    case TPUType::AtlasM:
        return "ATM_" + std::to_string(index);
    case TPUType::Unknown:
        return "TPU_" + std::to_string(index);
    }
    return "TPU_" + std::to_string(index);
}

}

DeviceManager::DeviceManager(HalType hal_type)
    : hal_(hal_type),
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

    // Ask the selected HAL context for observed PCI devices.
    auto pci_devices = hal_.scan_pci_devices();

    std::unordered_map<std::string, uint32_t> discovered_product_counts;

    for (auto& pci_device : pci_devices) {
        // FPGA bring-up intentionally matches chips by VID/DID only because
        // the platform and BDF assignment are not stable yet. Silicon-system
        // discovery will additionally bind VID/DID/BDF to a stable ATLAS_n.
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
        if (!policy_entry->device_config.pcie_modules.empty()) {
            const auto& pcie = policy_entry->device_config.pcie_modules.front();
            mapped_ctx.pcie_control_bar_index = pcie.bar_index;
            mapped_ctx.pcie_control_base = pcie.reg_base_offset;
        }

        // Bind operation implementations for the matched TPU type.
        auto implementer = std::make_shared<Implementer>(policy_entry->tpu_type);

        auto device_config = policy_entry->device_config;
        auto instance_index = discovered_product_counts[policy_entry->product]++;
        if (device_config.name.empty() ||
            target_registry_.find(device_config.name) != target_registry_.end()) {
            device_config.name = default_tpu_name(policy_entry->tpu_type, instance_index);
            device_config.tpu_index = instance_index;
        }

        // Build the TPU object and policy-defined child modules.
        auto tpu_device = std::make_unique<TPUDevice>(
            device_config.name,
            mapped_ctx,
            device_config,
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
    //   ATLAS_0     -> ATLAS_0
    //   PCIE_0_0    -> ATLAS_0
    //   DDP_0_1     -> ATLAS_0
    //   DMC_0_1_2   -> ATLAS_0
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
