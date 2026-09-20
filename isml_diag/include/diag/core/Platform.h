#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class TPUType { Unknown, Atlas, AtlasM };

class Logger;

inline std::string to_string(TPUType tpu_type)
{
    switch (tpu_type) {
    case TPUType::Atlas: return "atlas";
    case TPUType::AtlasM: return "atlas_m";
    case TPUType::Unknown: return "unknown";
    }
    return "unknown";
}

struct BarMapping {
    uint32_t bar_index = 0;
    std::string name;
    void* mapped_base = nullptr;
    uint64_t device_base = 0;
    uint64_t size = 0;
    uint64_t expected_size = 0;
    uint64_t resource_size = 0;
    uint64_t mapped_size = 0;
    bool mapped = false;
    bool writable = false;
    std::string error;
    std::vector<std::string> layout;
};

struct DeviceContext {
    std::string bdf;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;
    TPUType tpu_type = TPUType::Unknown;
    void* mapped_bar_base = nullptr;
    uint64_t bar_device_base = 0;
    uint64_t bar_size = 0;
    std::vector<BarMapping> bar_mappings;
    uint32_t pcie_control_bar_index = 0;
    uint64_t pcie_control_base = 0;

    // Module-relative register view; raw common::bar offsets remain unchanged.
    uint32_t reg_bar_index = 0;
    uint64_t reg_base_offset = 0;
    uint64_t reg_size = 0;
    uint64_t block_offset = 0;
    // Bring-up scratch region, independent of the module CSR address space.
    uint64_t dmem_base = 0x10000000;
    uint64_t dmem_size = 0x100000;
    void* phal = nullptr;
    void* pcie_phal = nullptr;
    Logger* logger = nullptr; // Borrowed only during a testcase.
    std::string target_name;
    // Copies passed to sibling modules share the physical device lock.
    std::shared_ptr<std::mutex> testcase_mutex = std::make_shared<std::mutex>();
};

struct ModuleInstanceConfig {
    uint32_t index = 0;
    uint32_t bar_index = 0;
    uint64_t reg_base_offset = 0;
    uint64_t reg_size = 0;
};

struct TPUDeviceConfig {
    std::string name;
    std::string slot;
    std::string position;
    uint32_t tpu_index = 0;
    std::vector<ModuleInstanceConfig> pcie_modules;
    std::vector<ModuleInstanceConfig> pmu_modules;
    std::vector<ModuleInstanceConfig> isi_modules;
    std::vector<ModuleInstanceConfig> ddp_modules;
};

struct PolicyEntry {
    uint16_t match_vendor_id = 0;
    uint16_t match_device_id = 0;
    std::string product;
    TPUType tpu_type = TPUType::Unknown;
    TPUDeviceConfig device_config;
};

std::vector<PolicyEntry> load_product_policy(const std::vector<std::string>& paths);
