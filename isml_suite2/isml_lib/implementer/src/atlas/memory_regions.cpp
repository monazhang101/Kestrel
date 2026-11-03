#include "diag/core/MemoryRegion.h"

#include <array>

namespace atlas_impl {
namespace {

constexpr std::array<DirectRcfRegion, 19> ATLAS_RCF_REGIONS{{
    {MemoryRegion::HQC_CORE_0_ILM, "HQC_CORE_0_ILM", 0, 0x36800000, 0x00080000, true},
    {MemoryRegion::HQC_CORE_0_DLM, "HQC_CORE_0_DLM", 0, 0x36880000, 0x00080000, true},
    {MemoryRegion::HQC_CORE_1_ILM, "HQC_CORE_1_ILM", 0, 0x36900000, 0x00080000, true},
    {MemoryRegion::HQC_CORE_1_DLM, "HQC_CORE_1_DLM", 0, 0x36980000, 0x00080000, true},
    {MemoryRegion::HQC_SRAM_0, "HQC_SRAM_0", 0, 0x36d00000, 0x00080000, true},
    {MemoryRegion::HQC_SRAM_1, "HQC_SRAM_1", 0, 0x36d80000, 0x00080000, true},
    {MemoryRegion::HQC_SRAM_2, "HQC_SRAM_2", 0, 0x36e00000, 0x00080000, true},
    {MemoryRegion::HQC_SRAM_3, "HQC_SRAM_3", 0, 0x36e80000, 0x00080000, true},
    {MemoryRegion::PMU_ILM, "PMU_ILM", 0, 0x39000800, 0x00040000, true},
    {MemoryRegion::PMU_DLM, "PMU_DLM", 0, 0x39040800, 0x00040000, true},
    {MemoryRegion::PMU_SRAM, "PMU_SRAM", 0, 0x3b000800, 0x00080000, true},
    {MemoryRegion::DDP0_ILM, "DDP0_ILM", 0, 0x32c00800, 0x00020000, true},
    {MemoryRegion::DDP0_DLM, "DDP0_DLM", 0, 0x32c40800, 0x00040000, true},
    {MemoryRegion::DDP1_ILM, "DDP1_ILM", 0, 0x33c00800, 0x00020000, true},
    {MemoryRegion::DDP1_DLM, "DDP1_DLM", 0, 0x33c40800, 0x00040000, true},
    {MemoryRegion::DDP2_ILM, "DDP2_ILM", 0, 0x34c00800, 0x00020000, true},
    {MemoryRegion::DDP2_DLM, "DDP2_DLM", 0, 0x34c40800, 0x00040000, true},
    {MemoryRegion::DDP3_ILM, "DDP3_ILM", 0, 0x35c00800, 0x00020000, true},
    {MemoryRegion::DDP3_DLM, "DDP3_DLM", 0, 0x35c40800, 0x00040000, true},
}};

constexpr std::array<DfRegion, 4> ATLAS_DF_REGIONS{{
    {MemoryRegion::AIC_SMEM, "AIC_SMEM", 0, 0, 0, 0, 0, 0, false},
    {MemoryRegion::AIC_IMEM, "AIC_IMEM", 0, 0, 0, 0, 0, 0, false},
    {MemoryRegion::AIC_VMEM, "AIC_VMEM", 0, 0, 0, 0, 0, 0, false},
    // 4 DDP x 3 DMC x 32 GiB = 384 GiB harvest space above the 0x10000000 base.
    // One 512 GiB window (BAR4-sized, power of two) covers it entirely.
    {MemoryRegion::DMEM, "DMEM",
        0x10000000,      // base
        0x8000000000,    // size
        0x8000000000,    // aperture_size
        4,               // default bar index
        0,               // default aperture index
        0,               // identity
        true},
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
