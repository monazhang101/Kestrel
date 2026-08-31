#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class TPUType {
    Unknown,
    Atlas,
    AtlasM,
};

inline std::string to_string(TPUType tpu_type)
{
    switch (tpu_type) {
    case TPUType::Atlas:
        return "atlas";
    case TPUType::AtlasM:
        return "atlas_m";
    case TPUType::Unknown:
        return "unknown";
    }
    return "unknown";
}

struct ModuleInstanceConfig {
    uint32_t index = 0;
    uint32_t bar_index = 0;
    uint64_t reg_offset = 0;
    uint64_t reg_size = 0;
};

struct DDPModuleConfig {
    uint32_t index = 0;
    uint32_t bar_index = 0;
    uint64_t reg_offset = 0;
    uint64_t reg_size = 0;
    std::vector<ModuleInstanceConfig> dmc_modules;
};

struct TPUDeviceConfig {
    std::string name;
    std::string slot;
    std::string position;
    uint32_t tpu_index = 0;

    std::vector<ModuleInstanceConfig> pcie_modules;
    std::vector<ModuleInstanceConfig> pmu_modules;
    std::vector<ModuleInstanceConfig> isi_modules;
    std::vector<DDPModuleConfig> ddp_modules;
};

struct PolicyEntry {
    uint16_t match_vendor_id = 0;
    uint16_t match_device_id = 0;
    std::string product;
    TPUType tpu_type = TPUType::Unknown;
    TPUDeviceConfig device_config;
};

std::vector<PolicyEntry> load_product_policy(const std::vector<std::string>& paths);
