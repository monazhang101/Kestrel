#include "diag/device/DeviceManager.h"

#include "diag/core/Format.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

DeviceManager::DeviceManager(const std::string& policy_dir)
    : testcase_policies_(load_testcase_policies(
          (std::filesystem::path(policy_dir) / "testcases").string())),
      policy_(load_product_policy({
          (std::filesystem::path(policy_dir) / "product" / "atlas_ubb.yaml").string(),
          (std::filesystem::path(policy_dir) / "product" / "atlas_m.yaml").string(),
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

    device_tree_.devices.push_back(std::move(info));
    device_tree_.topology.push_back({parent.get_name(), child.get_name(), "module"});

    for (auto& candidate : device_tree_.devices) {
        if (candidate.name == parent.get_name()) {
            candidate.children.push_back(child.get_name());
            break;
        }
    }

    for (const auto* grandchild : child.child_targets()) {
        if (grandchild != nullptr) {
            add_child_to_tree(*grandchild, child);
        }
    }
}

void DeviceManager::add_inventory_node(
    const TPUDevice& tpu,
    const std::string& parent,
    const std::string& name,
    const std::string& type,
    std::map<std::string, std::string> properties)
{
    const auto& ctx = tpu.get_context();
    DeviceDiscoveryInfo info;
    info.name = name;
    info.type = type;
    info.parent = parent;
    info.bdf = ctx.bdf;
    info.vendor_id = ctx.vendor_id;
    info.device_id = ctx.device_id;
    info.locator = {{"parent", parent}};
    info.properties = std::move(properties);
    device_tree_.devices.push_back(std::move(info));
    device_tree_.topology.push_back({parent, name, "module"});

    for (auto& candidate : device_tree_.devices) {
        if (candidate.name == parent) {
            candidate.children.push_back(name);
            break;
        }
    }
}

void DeviceManager::add_atlas_inventory_tree(const TPUDevice& tpu)
{
    const auto& resource = tpu.resource_info();
    const auto index = tpu.tpu_index();
    const auto tpu_name = tpu.get_name();
    const auto prefix = std::to_string(index) + "_";

    if (tpu.pcie() != nullptr) {
        add_inventory_node(tpu, tpu_name, tpu.pcie()->get_name(), "pcie");
    }
    if (tpu.pmu() != nullptr) {
        add_inventory_node(tpu, tpu_name, tpu.pmu()->get_name(), "pmu");
    }
    add_inventory_node(tpu, tpu_name, "NOC_" + std::to_string(index), "noc");
    add_inventory_node(tpu, tpu_name, "ACC_" + std::to_string(index), "acc");

    if (resource.has_aic_extension) {
        const auto& aic = resource.aic;
        std::map<std::string, std::string> properties{
            {"cfg", std::to_string(aic.cfg_code)},
            {"profile", to_string(aic.profile)},
            {"mask", common::format::hex(aic.block_mask, 2)},
            {"vpe", std::to_string(aic.vpe_count)},
            {"tpe", std::to_string(aic.tpe_count)},
            {"lpe", std::to_string(aic.lpe_count)},
        };
        if (aic.vpe_lanes != 0) {
            properties["vpe_lanes"] = std::to_string(aic.vpe_lanes);
        }
        if (aic.vpe_memory_mib != 0) {
            properties["vpe_memory_mib"] = std::to_string(aic.vpe_memory_mib);
        }
        add_inventory_node(tpu, tpu_name,
                           "AIC_" + std::to_string(index),
                           "aic", std::move(properties));
    }

    for (uint32_t ddp = 0; ddp < ATLAS_DDP_MAX; ++ddp) {
        const auto ddp_name = "DDP_" + prefix + std::to_string(ddp);
        add_inventory_node(tpu, tpu_name, ddp_name, "ddp");
        for (uint32_t dmc = 0; dmc < ATLAS_DMC_PER_DDP; ++dmc) {
            if (!resource.has_dmc(ddp, dmc)) continue;

            const auto type = resource.get_dmc_type(ddp, dmc);
            std::map<std::string, std::string> properties{
                {"type", to_string(type)},
            };
            if (type == AtlasDmcType::FakeDmem) {
                properties["size_gib"] =
                    std::to_string(resource.fake_dmem_size_bytes >> 30);
            }
            add_inventory_node(
                tpu, ddp_name,
                "DMC_" + prefix + std::to_string(ddp) + "_" +
                    std::to_string(dmc),
                "dmc", std::move(properties));
        }
    }

    for (const auto isi : resource.isi_indices()) {
        add_inventory_node(
            tpu, tpu_name,
            "ISI_" + prefix + std::to_string(isi),
            "isi", {{"index", std::to_string(isi)}});
    }
}

TestStatus DeviceManager::wait_system_ready(uint32_t timeout_ms,
                                            uint32_t poll_interval_ms)
{
    if (!hal_.write_bar32(0, ATLAS_PMU_UNLOCK_BAR0_OFFSET,
                          ATLAS_PMU_UNLOCK_KEY)) {
        logger_.error(
            "unlock Atlas PMU registers failed: offset=" +
            common::format::hex(ATLAS_PMU_UNLOCK_BAR0_OFFSET));
        return PHAL_STATUS_ERROR;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    uint32_t value = 0;
    do {
        if (!hal_.read_bar32(0, ATLAS_FW_STATUS_BAR0_OFFSET, value)) {
            logger_.error(
                "read Atlas system-ready register failed: offset=" +
                common::format::hex(ATLAS_FW_STATUS_BAR0_OFFSET));
            return PHAL_STATUS_ERROR;
        }
        if (value == ATLAS_FW_STATUS_IP_INIT_DONE) {
            return PHAL_STATUS_OK;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    } while (true);

    logger_.error(
        "wait Atlas system ready timed out: expected=" +
        common::format::hex(ATLAS_FW_STATUS_IP_INIT_DONE, 8) +
        " last=" + common::format::hex(value, 8));
    return PHAL_STATUS_TIMEOUT;
}

TestStatus DeviceManager::read_atlas_resource_info(AtlasResourceInfo& info)
{
    info = {};
    uint32_t raw = 0;
    if (!hal_.read_bar32(0, ATLAS_RESOURCE_INFO_BAR0_OFFSET, raw)) {
        logger_.error(
            "read Atlas resource-info register failed: offset=" +
            common::format::hex(ATLAS_RESOURCE_INFO_BAR0_OFFSET));
        return PHAL_STATUS_ERROR;
    }
    info = decode_atlas_presilicon_resource(raw);
    if (!info.valid) {
        logger_.error("invalid Atlas resource info: raw=" +
                      common::format::hex(raw, 8));
        return PHAL_STATUS_INVALID;
    }
    if (!info.profile_matched) {
        logger_.info("Atlas resource profile is not matched; using the basic "
                     "discover tree: raw=" + common::format::hex(raw, 8));
    }
    return PHAL_STATUS_OK;
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

        AtlasResourceInfo resource_info;
        if (policy_entry->tpu_type == TPUType::Atlas) {
            hal_.bind_device(mapped_ctx, &logger_);
            if (wait_system_ready() != PHAL_STATUS_OK) {
                continue;
            }
            if (read_atlas_resource_info(resource_info) != PHAL_STATUS_OK) {
                continue;
            }
        }

        auto device_config = policy_entry->device_config;
        device_config.tpu_index = static_cast<uint32_t>(devices_.size());
        device_config.name = "TPU" + std::to_string(device_config.tpu_index);
        if (resource_info.profile_matched) {
            device_config.isi_modules.erase(
                std::remove_if(device_config.isi_modules.begin(),
                               device_config.isi_modules.end(),
                               [&resource_info](const ModuleInstanceConfig& module) {
                                   return !resource_info.has_isi(module.index);
                               }),
                device_config.isi_modules.end());
        }

        // Build the TPU object and policy-defined child modules.
        auto tpu_device = std::make_unique<TPUDevice>(
            device_config.name,
            mapped_ctx,
            device_config,
            implementer,
            resource_info);

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
        if (resource_info.valid) {
            info.properties = {
                {"resource_raw", common::format::hex(resource_info.raw, 8)},
                {"profile", resource_info.profile_name},
                {"profile_matched",
                 resource_info.profile_matched ? "true" : "false"},
                {"platform", to_string(resource_info.platform)},
                {"jtag", to_string(resource_info.jtag_mux)},
                {"fake_dmem_size_gib",
                 std::to_string(resource_info.fake_dmem_size_bytes >> 30)},
                {"isi_mask", common::format::hex(resource_info.isi_mask, 2)},
                {"aic_mask", common::format::hex(resource_info.aic_mask, 2)},
                {"dmc_mask", common::format::hex(resource_info.dmc_mask, 3)},
                {"cxl_mask", common::format::hex(resource_info.cxl_mask, 3)},
            };
        }

        device_tree_.devices.push_back(std::move(info));
        device_tree_.topology.push_back({"PCIeRootComplex0", tpu->get_name(), "pcie"});

        if (resource_info.profile_matched) {
            add_atlas_inventory_tree(*tpu);
        } else {
            for (const auto* child : tpu->child_targets()) {
                if (child != nullptr) {
                    add_child_to_tree(*child, *tpu);
                }
            }
        }
        devices_.push_back(std::move(tpu_device));
    }

    return device_tree_;
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

    return target->run_testcase(test_name, args, &logger_, &hal_);
}

std::vector<TestArgs> DeviceManager::get_testcase_runs(const std::string& test_name) const
{
    const auto separator = test_name.find('_');
    if (separator == std::string::npos) return {};

    const auto target_policies = testcase_policies_.find(test_name.substr(0, separator));
    if (target_policies == testcase_policies_.end()) return {};

    const auto policy = target_policies->second.find(test_name.substr(separator + 1));
    return policy == target_policies->second.end() ? std::vector<TestArgs>{}
                                                   : policy->second.runs;
}

void DeviceManager::print_tree() const
{
    const auto find_device = [this](const std::string& name)
        -> const DeviceDiscoveryInfo* {
        for (const auto& device : device_tree_.devices) {
            if (device.name == name) return &device;
        }
        return nullptr;
    };
    const auto print_properties = [](const DeviceDiscoveryInfo& device) {
        if (device.properties.empty()) return;
        std::cout << " [";
        bool first = true;
        for (const auto& property : device.properties) {
            if (!first) std::cout << ' ';
            std::cout << property.first << '=' << property.second;
            first = false;
        }
        std::cout << ']';
    };
    const auto print_children = [&](const auto& self,
                                    const DeviceDiscoveryInfo& parent,
                                    const std::string& prefix) -> void {
        for (size_t i = 0; i < parent.children.size(); ++i) {
            const auto* child = find_device(parent.children[i]);
            if (child == nullptr) continue;
            const bool last = i + 1 == parent.children.size();
            std::cout << prefix << (last ? "`-- " : "|-- ")
                      << child->name << " [" << child->type << ']';
            print_properties(*child);
            std::cout << std::endl;
            self(self, *child, prefix + (last ? "    " : "|   "));
        }
    };

    for (const auto& device : device_tree_.devices) {
        if (device.type != "tpu") continue;
        std::cout << device.name << " [TPU]"
                  << " bdf=" << device.bdf;
        print_properties(device);
        std::cout << std::endl;
        print_children(print_children, device, "");
    }
}
