#include "diag/core/MemoryRegion.h"

#include <array>

namespace atlas_impl {
namespace {

constexpr std::array<DirectRcfRegion, 13> ATLAS_RCF_REGIONS{{
    {MemoryRegion::HQC_CORE_0_ILM, "HQC_CORE_0_ILM", "pcie", 0x00800000, 0x00080000, true},
    {MemoryRegion::HQC_CORE_0_DLM, "HQC_CORE_0_DLM", "pcie", 0x00880000, 0x00080000, true},
    {MemoryRegion::HQC_CORE_1_ILM, "HQC_CORE_1_ILM", "pcie", 0x00900000, 0x00080000, true},
    {MemoryRegion::HQC_CORE_1_DLM, "HQC_CORE_1_DLM", "pcie", 0x00980000, 0x00080000, true},
    {MemoryRegion::HQC_SRAM_0, "HQC_SRAM_0", "pcie", 0x00d00000, 0x00080000, true},
    {MemoryRegion::HQC_SRAM_1, "HQC_SRAM_1", "pcie", 0x00d80000, 0x00080000, true},
    {MemoryRegion::HQC_SRAM_2, "HQC_SRAM_2", "pcie", 0x00e00000, 0x00080000, true},
    {MemoryRegion::HQC_SRAM_3, "HQC_SRAM_3", "pcie", 0x00e80000, 0x00080000, true},
    {MemoryRegion::PMU_ILM, "PMU_ILM", "pmu", 0x01000800, 0x00040000, true},
    {MemoryRegion::PMU_DLM, "PMU_DLM", "pmu", 0x01040800, 0x00040000, true},
    {MemoryRegion::PMU_SRAM, "PMU_SRAM", "pmu", 0x03000800, 0x00080000, true},
    {MemoryRegion::DDP_ILM, "DDP_ILM", "ddp", 0x00c00800, 0x00020000, true},
    {MemoryRegion::DDP_DLM, "DDP_DLM", "ddp", 0x00c40800, 0x00040000, true},
}};

constexpr std::array<DfRegion, 4> ATLAS_DF_REGIONS{{
    {MemoryRegion::AIC_SMEM, "AIC_SMEM", 0, 0, 0, false},
    {MemoryRegion::AIC_IMEM, "AIC_IMEM", 0, 0, 0, false},
    {MemoryRegion::AIC_VMEM, "AIC_VMEM", 0, 0, 0, false},
    // Provisional 256 MiB allocatable range; physical capacity is not confirmed.
    {MemoryRegion::DMEM, "DMEM", 0x10000000, 0x10000000, 0x10000000, true},
}};

}

MemoryRegionMap memory_regions()
{
    return {
        {ATLAS_RCF_REGIONS.data(), ATLAS_RCF_REGIONS.size()},
        {ATLAS_DF_REGIONS.data(), ATLAS_DF_REGIONS.size()},
    };
}

}
