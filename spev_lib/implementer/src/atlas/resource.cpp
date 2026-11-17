#include "diag/implementer/AtlasResource.h"

#include <algorithm>
#include <iterator>

namespace {

constexpr uint64_t GIB = 1024ull * 1024ull * 1024ull;

const AtlasPresiliconResourceProfile* find_presilicon_profile(uint32_t raw)
{
    for (const auto& profile : ATLAS_EMU_RESOURCE_PROFILES) {
        if (profile.raw == raw) return &profile;
    }
    for (const auto& profile : ATLAS_FPGA_RESOURCE_PROFILES) {
        if (profile.raw == raw) return &profile;
    }
    return nullptr;
}

uint32_t all_tpe()
{
    return ATLAS_AIC_TPE0 | ATLAS_AIC_TPE1 |
           ATLAS_AIC_TPE2 | ATLAS_AIC_TPE3;
}

uint32_t all_lpe()
{
    return ATLAS_AIC_LPE0 | ATLAS_AIC_LPE1;
}

AtlasAicInfo decode_aic(uint8_t cfg, bool has_extension)
{
    AtlasAicInfo info;
    info.cfg_code = cfg;
    if (!has_extension) return info;

    switch (cfg) {
    case 1:
        info.profile = AtlasAicProfile::EmuCut1;
        info.block_mask = ATLAS_AIC_ACC | ATLAS_AIC_VPE |
                          ATLAS_AIC_TPE0 | ATLAS_AIC_LPE0;
        info.vpe_count = 1;
        info.vpe_lanes = 128;
        info.vpe_memory_mib = 32;
        info.tpe_count = 1;
        info.lpe_count = 1;
        break;
    case 2:
        info.profile = AtlasAicProfile::Cut2;
        info.block_mask = ATLAS_AIC_ACC | ATLAS_AIC_VPE |
                          all_tpe() | all_lpe();
        info.vpe_count = 1;
        info.vpe_lanes = 16;
        info.tpe_count = 4;
        info.lpe_count = 2;
        break;
    case 3:
        info.profile = AtlasAicProfile::FpgaCut3;
        info.block_mask = ATLAS_AIC_ACC | ATLAS_AIC_VPE |
                          ATLAS_AIC_TPE0 | all_lpe();
        info.vpe_count = 1;
        info.vpe_lanes = 16;
        info.tpe_count = 1;
        info.lpe_count = 2;
        break;
    case 4:
        info.profile = AtlasAicProfile::FpgaCut1;
        info.block_mask = ATLAS_AIC_ACC | ATLAS_AIC_VPE |
                          ATLAS_AIC_TPE0 | ATLAS_AIC_LPE0;
        info.vpe_count = 1;
        info.vpe_lanes = 1;
        info.tpe_count = 1;
        info.lpe_count = 1;
        break;
    case 5:
        info.profile = AtlasAicProfile::EmuFull;
        info.block_mask = ATLAS_AIC_ACC | ATLAS_AIC_VPE |
                          all_tpe() | all_lpe();
        info.vpe_count = 1;
        info.tpe_count = 4;
        info.lpe_count = 2;
        break;
    default:
        info.profile = AtlasAicProfile::Unknown;
        info.block_mask = ATLAS_AIC_ACC;
        break;
    }
    return info;
}

}

bool AtlasResourceInfo::has_isi(uint32_t index) const
{
    return valid && index < ATLAS_ISI_MAX &&
           (isi_mask & (1u << index)) != 0;
}

std::vector<uint32_t> AtlasResourceInfo::isi_indices() const
{
    std::vector<uint32_t> indices;
    for (uint32_t index = 0; index < ATLAS_ISI_MAX; ++index) {
        if (has_isi(index)) indices.push_back(index);
    }
    return indices;
}

bool AtlasResourceInfo::has_aic_blocks(uint32_t blocks) const
{
    return valid && (aic_mask & blocks) == blocks;
}

bool AtlasResourceInfo::has_dmc(uint32_t ddp, uint32_t dmc) const
{
    if (!valid || ddp >= ATLAS_DDP_MAX || dmc >= ATLAS_DMC_PER_DDP) {
        return false;
    }
    return (dmc_mask & (1u << (ddp * ATLAS_DMC_PER_DDP + dmc))) != 0;
}

bool AtlasResourceInfo::has_cxl(uint32_t ddp, uint32_t dmc) const
{
    if (!valid || ddp >= ATLAS_DDP_MAX || dmc >= ATLAS_DMC_PER_DDP) {
        return false;
    }
    return (cxl_mask & (1u << (ddp * ATLAS_DMC_PER_DDP + dmc))) != 0;
}

AtlasDmcType AtlasResourceInfo::get_dmc_type(uint32_t ddp, uint32_t dmc) const
{
    if (!valid || ddp >= ATLAS_DDP_MAX || dmc >= ATLAS_DMC_PER_DDP) {
        return AtlasDmcType::FakeDmem;
    }
    return dmc_type[ddp][dmc];
}

AtlasResourceInfo decode_atlas_presilicon_resource(uint32_t raw)
{
    AtlasResourceInfo info;
    info.raw = raw;
    for (auto& ddp : info.dmc_type) {
        std::fill(std::begin(ddp), std::end(ddp), AtlasDmcType::FakeDmem);
    }

    const auto platform = static_cast<uint8_t>(raw & 0xfu);
    info.subsys_cfg = static_cast<uint8_t>((raw >> 4) & 0xfu);
    info.cxl_cfg = static_cast<uint8_t>((raw >> 8) & 0xfu);
    info.isi_cfg = static_cast<uint8_t>((raw >> 12) & 0xfu);
    info.aic_cfg = static_cast<uint8_t>((raw >> 16) & 0xfu);
    info.jtag_cfg = static_cast<uint8_t>((raw >> 20) & 0x3u);
    info.dmem_size_cfg = static_cast<uint8_t>((raw >> 22) & 0x3u);

    switch (platform) {
    case 0: info.platform = AtlasPlatform::Chip; break;
    case 1: info.platform = AtlasPlatform::Emu; break;
    case 2: info.platform = AtlasPlatform::Fpga; break;
    default: info.platform = AtlasPlatform::Unknown; break;
    }
    switch (info.jtag_cfg) {
    case 0: info.jtag_mux = AtlasJtagMux::Pmu; break;
    case 1: info.jtag_mux = AtlasJtagMux::Chain1; break;
    case 2: info.jtag_mux = AtlasJtagMux::Chain4; break;
    default: info.jtag_mux = AtlasJtagMux::Unknown; break;
    }
    if (info.dmem_size_cfg == 0) {
        info.fake_dmem_size_bytes =
            (info.platform == AtlasPlatform::Fpga ? 8ull : 32ull) * GIB;
    } else if (info.dmem_size_cfg == 1 &&
               info.platform != AtlasPlatform::Fpga) {
        info.fake_dmem_size_bytes = 64ull * GIB;
    }

    info.has_cxl_subsystem = info.subsys_cfg == 1 || info.subsys_cfg == 5;
    const bool has_isi_subsystem = info.subsys_cfg == 2 || info.subsys_cfg == 4;
    info.has_aic_extension = info.subsys_cfg == 3 ||
                             info.subsys_cfg == 4 ||
                             info.subsys_cfg == 5;

    const bool known_header = raw != 0u && raw != 0xffffffffu &&
        info.platform != AtlasPlatform::Unknown && info.subsys_cfg <= 5 &&
        info.jtag_mux != AtlasJtagMux::Unknown &&
        info.fake_dmem_size_bytes != 0;
    const bool known_details =
        (!info.has_cxl_subsystem || info.cxl_cfg <= 3) &&
        (!has_isi_subsystem || info.isi_cfg <= 1) &&
        (!info.has_aic_extension ||
         (info.aic_cfg >= 1 && info.aic_cfg <= 5));
    info.valid = known_header && known_details;
    if (!info.valid) return info;

    const auto* profile = find_presilicon_profile(raw);
    info.profile_matched = profile != nullptr;
    if (profile != nullptr) {
        info.profile_name = profile->name;
        info.dmc_mask = profile->dmc_mask;
    } else {
        info.profile_name = "unmatched";
        if (info.platform == AtlasPlatform::Emu) {
            info.dmc_mask = ATLAS_ALL_DMC_MASK;
        } else if (info.platform == AtlasPlatform::Fpga) {
            info.dmc_mask = ATLAS_DMC0_MASK;
        }
    }

    if (has_isi_subsystem && info.isi_cfg == 0) info.isi_mask = 0x03u;
    else if (has_isi_subsystem && info.isi_cfg == 1) info.isi_mask = 0x33u;

    info.aic = decode_aic(info.aic_cfg, info.has_aic_extension);
    info.aic_mask = info.aic.block_mask;

    if (info.has_cxl_subsystem) {
        if (info.cxl_cfg == 1) {
            info.dmc_type[0][0] = AtlasDmcType::Cxl30;
        } else if (info.cxl_cfg == 2) {
            info.dmc_type[0][0] = AtlasDmcType::Cxl30;
            info.dmc_type[0][1] = AtlasDmcType::Cxl30;
            info.dmc_type[0][2] = AtlasDmcType::Cxl20;
        } else if (info.cxl_cfg == 3) {
            info.dmc_type[0][0] = AtlasDmcType::Cxl30;
            info.dmc_type[1][0] = AtlasDmcType::Cxl30;
            info.dmc_type[2][0] = AtlasDmcType::Cxl20;
            info.dmc_type[3][0] = AtlasDmcType::Cxl20;
        }
    }
    for (uint32_t ddp = 0; ddp < ATLAS_DDP_MAX; ++ddp) {
        for (uint32_t dmc = 0; dmc < ATLAS_DMC_PER_DDP; ++dmc) {
            if (info.dmc_type[ddp][dmc] != AtlasDmcType::FakeDmem) {
                const auto bit = 1u << (ddp * ATLAS_DMC_PER_DDP + dmc);
                info.dmc_mask |= bit;
                info.cxl_mask |= bit;
            }
        }
    }
    return info;
}

std::string to_string(AtlasPlatform platform)
{
    switch (platform) {
    case AtlasPlatform::Chip: return "chip";
    case AtlasPlatform::Emu: return "emu";
    case AtlasPlatform::Fpga: return "fpga";
    case AtlasPlatform::Unknown: return "unknown";
    }
    return "unknown";
}

std::string to_string(AtlasDmcType type)
{
    switch (type) {
    case AtlasDmcType::FakeDmem: return "fake-dmem";
    case AtlasDmcType::Cxl20: return "cxl2.0";
    case AtlasDmcType::Cxl30: return "cxl3.0";
    }
    return "unknown";
}

std::string to_string(AtlasJtagMux mux)
{
    switch (mux) {
    case AtlasJtagMux::Pmu: return "pmu";
    case AtlasJtagMux::Chain1: return "chain1";
    case AtlasJtagMux::Chain4: return "chain4";
    case AtlasJtagMux::Unknown: return "unknown";
    }
    return "unknown";
}

std::string to_string(AtlasAicProfile profile)
{
    switch (profile) {
    case AtlasAicProfile::AccOnly: return "acc-only";
    case AtlasAicProfile::EmuCut1: return "emu-cut1";
    case AtlasAicProfile::Cut2: return "cut2";
    case AtlasAicProfile::FpgaCut3: return "fpga-cut3";
    case AtlasAicProfile::FpgaCut1: return "fpga-cut1";
    case AtlasAicProfile::EmuFull: return "emu-full";
    case AtlasAicProfile::Unknown: return "unknown";
    }
    return "unknown";
}
