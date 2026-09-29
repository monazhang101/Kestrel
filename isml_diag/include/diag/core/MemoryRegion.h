#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

enum class MemoryRegion {
    DMEM,
    HQC_CORE_0_ILM,
    HQC_CORE_0_DLM,
    HQC_CORE_1_ILM,
    HQC_CORE_1_DLM,
    HQC_SRAM_0,
    HQC_SRAM_1,
    HQC_SRAM_2,
    HQC_SRAM_3,
    PMU_ILM,
    PMU_DLM,
    PMU_SRAM,
    DDP0_ILM,
    DDP0_DLM,
    DDP1_ILM,
    DDP1_DLM,
    DDP2_ILM,
    DDP2_DLM,
    DDP3_ILM,
    DDP3_DLM,
    AIC_SMEM,
    AIC_IMEM,
    AIC_VMEM,
};

inline constexpr MemoryRegion DMEM = MemoryRegion::DMEM;
inline constexpr MemoryRegion HQC_CORE_0_ILM = MemoryRegion::HQC_CORE_0_ILM;
inline constexpr MemoryRegion HQC_CORE_0_DLM = MemoryRegion::HQC_CORE_0_DLM;
inline constexpr MemoryRegion HQC_CORE_1_ILM = MemoryRegion::HQC_CORE_1_ILM;
inline constexpr MemoryRegion HQC_CORE_1_DLM = MemoryRegion::HQC_CORE_1_DLM;
inline constexpr MemoryRegion HQC_SRAM_0 = MemoryRegion::HQC_SRAM_0;
inline constexpr MemoryRegion HQC_SRAM_1 = MemoryRegion::HQC_SRAM_1;
inline constexpr MemoryRegion HQC_SRAM_2 = MemoryRegion::HQC_SRAM_2;
inline constexpr MemoryRegion HQC_SRAM_3 = MemoryRegion::HQC_SRAM_3;
inline constexpr MemoryRegion PMU_ILM = MemoryRegion::PMU_ILM;
inline constexpr MemoryRegion PMU_DLM = MemoryRegion::PMU_DLM;
inline constexpr MemoryRegion PMU_SRAM = MemoryRegion::PMU_SRAM;
inline constexpr MemoryRegion DDP0_ILM = MemoryRegion::DDP0_ILM;
inline constexpr MemoryRegion DDP0_DLM = MemoryRegion::DDP0_DLM;
inline constexpr MemoryRegion DDP1_ILM = MemoryRegion::DDP1_ILM;
inline constexpr MemoryRegion DDP1_DLM = MemoryRegion::DDP1_DLM;
inline constexpr MemoryRegion DDP2_ILM = MemoryRegion::DDP2_ILM;
inline constexpr MemoryRegion DDP2_DLM = MemoryRegion::DDP2_DLM;
inline constexpr MemoryRegion DDP3_ILM = MemoryRegion::DDP3_ILM;
inline constexpr MemoryRegion DDP3_DLM = MemoryRegion::DDP3_DLM;
inline constexpr MemoryRegion AIC_SMEM = MemoryRegion::AIC_SMEM;
inline constexpr MemoryRegion AIC_IMEM = MemoryRegion::AIC_IMEM;
inline constexpr MemoryRegion AIC_VMEM = MemoryRegion::AIC_VMEM;

struct DirectRcfRegion {
    MemoryRegion id;
    const char* name;
    uint32_t bar_index;
    uint64_t bar_offset; // Absolute BAR-relative address in the chip map.
    uint64_t size;
    bool supported;
};

struct DirectRcfRegionMap {
    const DirectRcfRegion* regions = nullptr;
    size_t size = 0;
};

struct DfRegion {
    MemoryRegion id;
    const char* name;
    uint64_t base; // Device-memory address-space base.
    uint64_t size; // Allocatable capacity, not the aperture capacity.
    uint64_t aperture_size;
    uint32_t bar_index;
    uint8_t aperture_index;
    uint8_t identity;
    bool supported;
};

struct DfRegionMap {
    const DfRegion* regions = nullptr;
    size_t size = 0;
};

struct MemoryRegionMap {
    DirectRcfRegionMap rcf;
    DfRegionMap df;
};

// One state per physical TPU, shared by all module contexts. Occupied ranges
// are ordered by offset; gaps are reused on the next allocation.
struct DeviceMemoryState {
    using Key = MemoryRegion;
    std::mutex allocation_mutex;
    std::mutex aperture_mutex;
    std::map<Key, std::map<uint64_t, uint64_t>> allocations;
};

inline const DirectRcfRegion* find_rcf_region(const DirectRcfRegionMap& map,
                                               MemoryRegion id)
{
    for (size_t i = 0; i < map.size; ++i) {
        if (map.regions[i].id == id) return &map.regions[i];
    }
    return nullptr;
}

inline const DfRegion* find_df_region(const DfRegionMap& map, MemoryRegion id)
{
    for (size_t i = 0; i < map.size; ++i) {
        if (map.regions[i].id == id) return &map.regions[i];
    }
    return nullptr;
}
