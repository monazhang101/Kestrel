#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <atlas_csr_sw/src/h/TOP.h>
#include <atlas_csr_sw/src/h/TOP_deps/PMU_CORE/PMU_CORE.h>
#include <atlas_csr_sw/src/h/TOP_deps/PMU_CORE/PMU_DIAG_REG/PMU_DIAG_REG.h>
#include <atlas_csr_sw/src/h/TOP_deps/PMU_CORE/PMU_RCF_SA/PMU_RCF_SA.h>
#include <atlas_csr_sw/src/h/TOP_deps/PMU_CORE/PMU_SYS_REG/PMU_SYS_REG.h>

inline constexpr uint32_t ATLAS_ISI_MAX = 8;
inline constexpr uint32_t ATLAS_DDP_MAX = 4;
inline constexpr uint32_t ATLAS_DMC_PER_DDP = 3;
inline constexpr uint32_t ATLAS_DMC0_MASK = 0x249u;
inline constexpr uint32_t ATLAS_ALL_DMC_MASK = 0xfffu;

inline constexpr uint32_t ATLAS_AIC_ACC = 1u << 0;
inline constexpr uint32_t ATLAS_AIC_VPE = 1u << 1;
inline constexpr uint32_t ATLAS_AIC_TPE0 = 1u << 2;
inline constexpr uint32_t ATLAS_AIC_TPE1 = 1u << 3;
inline constexpr uint32_t ATLAS_AIC_TPE2 = 1u << 4;
inline constexpr uint32_t ATLAS_AIC_TPE3 = 1u << 5;
inline constexpr uint32_t ATLAS_AIC_LPE0 = 1u << 6;
inline constexpr uint32_t ATLAS_AIC_LPE1 = 1u << 7;

// BAR0 offsets in the host-visible Atlas address space.
inline constexpr uint64_t ATLAS_BAR0_RCF_BASE = 0x20000000ull;
inline constexpr uint64_t ATLAS_PMU_INTERNAL_BASE = TOP_U_PMU_CORE_0_OFFSET;
inline constexpr uint64_t ATLAS_PMU_BASE =
    ATLAS_BAR0_RCF_BASE + ATLAS_PMU_INTERNAL_BASE;

inline constexpr uint64_t ATLAS_PMU_UNLOCK_BAR0_OFFSET =
    ATLAS_PMU_BASE + PMU_CORE_U_PMU_RCF_SA_0_OFFSET +
    PMU_RCF_SA_PMU_LOCK_OFFSET;
inline constexpr uint32_t ATLAS_PMU_UNLOCK_KEY = 0x5a5aa5a5u;

// DB/RCF-SA window shift applied between the PMU base and the PMU register
// blocks; the CSR headers do not model this translation.
inline constexpr uint64_t ATLAS_PMU_REG_WINDOW = 0x800ull;

// CSR names SYSREG15 and REG15 are generic; the host-FW contract assigns the
// system-ready status and resource inventory meanings below.
inline constexpr uint64_t ATLAS_FW_STATUS_BAR0_OFFSET =
    ATLAS_PMU_BASE + ATLAS_PMU_REG_WINDOW +
    PMU_CORE_U_PMU_SYS_REG_0_OFFSET + PMU_SYS_REG_SYSREG15_OFFSET;
inline constexpr uint64_t ATLAS_RESOURCE_INFO_BAR0_OFFSET =
    ATLAS_PMU_BASE + ATLAS_PMU_REG_WINDOW +
    PMU_CORE_U_PMU_DIAG_REG_0_OFFSET + PMU_DIAG_REG_REG15_OFFSET;
inline constexpr uint32_t ATLAS_FW_STATUS_IP_INIT_DONE = 0x5b5b5b5bu;

enum class AtlasPlatform : uint8_t {
    Chip = 0,
    Emu = 1,
    Fpga = 2,
    Unknown = 0xff,
};

enum class AtlasDmcType : uint8_t {
    FakeDmem = 0,
    Cxl20 = 1,
    Cxl30 = 2,
};

enum class AtlasJtagMux : uint8_t {
    Pmu = 0,
    Chain1 = 1,
    Chain4 = 2,
    Unknown = 0xff,
};

enum class AtlasAicProfile : uint8_t {
    AccOnly,
    EmuCut1,
    Cut2,
    FpgaCut3,
    FpgaCut1,
    EmuFull,
    Unknown,
};

struct AtlasAicInfo {
    uint8_t cfg_code = 0;
    AtlasAicProfile profile = AtlasAicProfile::AccOnly;
    uint32_t block_mask = ATLAS_AIC_ACC;
    uint32_t vpe_count = 0;
    uint32_t vpe_lanes = 0;
    uint32_t vpe_memory_mib = 0;
    uint32_t tpe_count = 0;
    uint32_t lpe_count = 0;
};

struct AtlasPresiliconResourceProfile {
    uint32_t raw;
    const char* name;
    uint32_t dmc_mask;
};

// Known exact DB values used to present the presilicon inventory tree. The
// decoder still exposes fields from structurally valid unmatched DB values.
inline constexpr std::array<AtlasPresiliconResourceProfile, 10>
    ATLAS_EMU_RESOURCE_PROFILES{{
        {0x000001u, "base1", ATLAS_ALL_DMC_MASK},
        {0x400001u, "base2", ATLAS_ALL_DMC_MASK},
        {0x010031u, "aic-cut1", ATLAS_ALL_DMC_MASK},
        {0x020031u, "aic-cut2", ATLAS_ALL_DMC_MASK},
        {0x050031u, "aic-cut3", ATLAS_ALL_DMC_MASK},
        {0x000021u, "isi", ATLAS_ALL_DMC_MASK},
        {0x000111u, "cxl1", ATLAS_ALL_DMC_MASK},
        {0x000211u, "cxl2", ATLAS_ALL_DMC_MASK},
        {0x000311u, "cxl3", ATLAS_ALL_DMC_MASK},
        {0x020151u, "cxl4", ATLAS_ALL_DMC_MASK},
    }};

inline constexpr std::array<AtlasPresiliconResourceProfile, 6>
    ATLAS_FPGA_RESOURCE_PROFILES{{
        {0x000002u, "base", ATLAS_DMC0_MASK},
        {0x040032u, "aic-cut1", ATLAS_DMC0_MASK},
        {0x020032u, "aic-cut2", ATLAS_DMC0_MASK},
        {0x030032u, "aic-cut3", ATLAS_DMC0_MASK},
        {0x001022u, "isi", ATLAS_DMC0_MASK},
        {0x031042u, "isi-aic-cut3", ATLAS_DMC0_MASK},
    }};

struct AtlasResourceInfo {
    uint32_t raw = 0;
    bool valid = false;
    bool profile_matched = false;
    std::string profile_name;
    AtlasPlatform platform = AtlasPlatform::Unknown;
    uint8_t subsys_cfg = 0;
    uint8_t cxl_cfg = 0;
    uint8_t isi_cfg = 0;
    uint8_t aic_cfg = 0;
    uint8_t jtag_cfg = 0;
    uint8_t dmem_size_cfg = 0;
    bool has_cxl_subsystem = false;
    bool has_aic_extension = false;
    uint32_t isi_mask = 0;
    uint32_t aic_mask = ATLAS_AIC_ACC;
    // One bit per DMC location: bit = ddp_index * 3 + dmc_index.
    uint32_t dmc_mask = 0;
    uint32_t cxl_mask = 0;
    AtlasDmcType dmc_type[ATLAS_DDP_MAX][ATLAS_DMC_PER_DDP]{};
    // Size of each fake DMEM: FPGA cfg0=8 GiB; EMU cfg0/1=32/64 GiB.
    uint64_t fake_dmem_size_bytes = 0;
    AtlasJtagMux jtag_mux = AtlasJtagMux::Unknown;
    AtlasAicInfo aic;

    bool has_isi(uint32_t index) const;
    std::vector<uint32_t> isi_indices() const;
    bool has_aic_blocks(uint32_t blocks) const;
    bool has_dmc(uint32_t ddp, uint32_t dmc) const;
    bool has_cxl(uint32_t ddp, uint32_t dmc) const;
    AtlasDmcType get_dmc_type(uint32_t ddp, uint32_t dmc) const;
};

AtlasResourceInfo decode_atlas_presilicon_resource(uint32_t raw);

std::string to_string(AtlasPlatform platform);
std::string to_string(AtlasDmcType type);
std::string to_string(AtlasJtagMux mux);
std::string to_string(AtlasAicProfile profile);
