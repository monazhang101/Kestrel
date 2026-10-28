#include "diag/modules/PCIeModule.h"
#include "diag/core/Common.h"
#include "diag/core/PhalBridge.h"
#include "diag/device/TPUDevice.h"
#include "diag/implementer/HqcCpQueue.h"
#include "diag/implementer/HqcDma.h"

extern "C" {
#include <phal/phal.h>
#if __has_include(<phal/ops.h>)
#include <phal/ops.h>
#endif
#include <atlas_csr_sw/src/h/TOP.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP_ASC_HASH_REG/DDP_ASC_HASH_REG.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP_NDM_REG/DDP_NDM_REG.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/PCIE.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/PCIE_APP_REG_FOR_AXICLK/PCIE_APP_REG_FOR_AXICLK.h>
}

#include <array>
#include <cstring>

using common::format::hex;

TestStatus PCIeModule::dma_h2d_example(TestInfo& ti)
{
    constexpr uint64_t SIZE_BYTES = 32;
    constexpr uint64_t TIMEOUT_MS = 1000;

    if (ti.hal == nullptr) {
        ti.logger->error("DMA example requires a bound HAL context");
        return PHAL_STATUS_ERROR;
    }

    auto host = ti.hal->host_mem_alloc(SIZE_BYTES);
    auto device = ti.hal->device_mem_alloc(DMEM, SIZE_BYTES);
    if (!host.valid() || !device.valid()) {
        ti.logger->error("DMA example allocation failed");
        return PHAL_STATUS_ERROR;
    }

    const auto payload = common::pattern::generate(SIZE_BYTES, "incremental");
    if (payload.empty()) {
        ti.logger->error("DMA example pattern generation failed");
        return PHAL_STATUS_ERROR;
    }
    std::memcpy(host.cpu_base(), payload.data(), payload.size());

    HqcDmaEngine engine(*ti.hal);
    DmaDescriptor descriptor(engine, DmaProfile::H2D);
    auto status = descriptor.set_src(host);
    if (status == PHAL_STATUS_OK) status = descriptor.set_dst(device);
    if (status == PHAL_STATUS_OK) status = descriptor.set_size(SIZE_BYTES);
    if (status != PHAL_STATUS_OK) return status;

    ti.logger->info("DMA H2D example: execute one 32-byte host-to-DMEM transfer");
    return descriptor.execute(TIMEOUT_MS);
}

TestStatus PCIeModule::cp_h2sram_example(TestInfo& ti)
{
    constexpr uint64_t SIZE_BYTES = 512;
    constexpr uint64_t TIMEOUT_MS = 1000;

    if (ti.hal == nullptr) {
        ti.logger->error("CP example requires a bound HAL context");
        return PHAL_STATUS_ERROR;
    }

    auto host = ti.hal->host_mem_alloc(SIZE_BYTES);
    auto sram = ti.hal->device_mem_alloc(HQC_SRAM_2, SIZE_BYTES);
    if (!host.valid() || !sram.valid()) {
        ti.logger->error("CP example allocation failed");
        return PHAL_STATUS_ERROR;
    }

    const auto payload = common::pattern::generate(SIZE_BYTES, "incremental");
    if (payload.empty()) {
        ti.logger->error("CP example pattern generation failed");
        return PHAL_STATUS_ERROR;
    }
    std::memcpy(host.cpu_base(), payload.data(), payload.size());

    HqcCpQueue queue(*ti.hal);
    ti.logger->info("CP H2SRAM example: execute one 512-byte host-to-HQC-SRAM-bank-2 transfer");
    return queue.copy(host, sram, SIZE_BYTES, TIMEOUT_MS);
}

TestStatus PCIeModule::example(TestInfo& ti)
{
    if (device_ == nullptr) {
        ti.logger->error("PCIe example requires an owning TPU device");
        return PHAL_STATUS_ERROR;
    }

    const auto& resource = device_->resource_info();
    const auto isi_indices = device_->isi_indices();
    std::string isi_list = "{";
    for (size_t i = 0; i < isi_indices.size(); ++i) {
        if (i != 0) isi_list += ",";
        isi_list += std::to_string(isi_indices[i]);
    }
    isi_list += "}";

    ti.logger->info("Atlas inventory API example");
    ti.logger->info("  resource_raw=" + hex(resource.raw, 8) +
                    " profile=" + resource.profile_name +
                    " valid=" + (resource.valid ? "true" : "false") +
                    " profile_matched=" +
                    (resource.profile_matched ? "true" : "false"));
    ti.logger->info("  isi_indices=" + isi_list +
                    " has_isi(0)=" + (device_->has_isi(0) ? "true" : "false") +
                    " has_isi(4)=" + (device_->has_isi(4) ? "true" : "false"));
    ti.logger->info("  dmc_mask=" + hex(device_->dmc_mask(), 3) +
                    " has_dmc(0,0)=" +
                    (device_->has_dmc(0, 0) ? "true" : "false") +
                    " has_dmc(0,1)=" +
                    (device_->has_dmc(0, 1) ? "true" : "false"));
    ti.logger->info("  cxl_mask=" + hex(device_->cxl_mask(), 3) +
                    " has_cxl(0,0)=" +
                    (device_->has_cxl(0, 0) ? "true" : "false"));
    ti.logger->info("  dmc_type(0,0)=" +
                    to_string(device_->dmc_type(0, 0)) +
                    " fake_dmem_size_gib=" +
                    std::to_string(resource.fake_dmem_size_bytes >> 30));

    auto* phal_ctx = ti.phal_ctx;
    if (phal_ctx == nullptr) {
        ti.logger->error("chip-level PHAL context is not initialized");
        return PHAL_STATUS_ERROR;
    }

    uint32_t data = 0;
    uint32_t q128_en = 0;

    // ti.logger->info("env.base=" + hex(phal_ctx->env.base));
    phal_block_ctx_enter(phal_ctx, TOP_U_PCIE_0_OFFSET);
    // ti.logger->info("env.base=" + hex(phal_ctx->env.base));
    auto status = phal_read(phal_ctx, 0x100000, &data);
    status |= phal_read(phal_ctx, PCIE_U_PCIE_APP_REG_FOR_AXICLK_0_OFFSET + PCIE_APP_REG_FOR_AXICLK_Q128_ENABLE_OFFSET, &q128_en);
    phal_block_ctx_exit(phal_ctx, TOP_U_PCIE_0_OFFSET);
    // ti.logger->info("env.base=" + hex(phal_ctx->env.base));
    ti.logger->info("q128_en" + hex(q128_en));

    if (status != PHAL_STATUS_OK) return status;
    if (data != 0xabcd16c3) {
        ti.logger->error("PCIe DID/VID: expected 0xabcd16c3, actual=" + hex(data, 8));
        return PHAL_STATUS_ERROR;
    }


    // phal_ctx->env.base = TOP_U_CHIP_OFFSET;
    // TOP_U_DDP_0.U_DDP_ASC_HASH_REG_0.DMEM_SIZE
    uint32_t ddp_num = 4;
    uint32_t ndm_ctrl = 0;
    const uint32_t ddp_reg_offset[] = {TOP_U_DDP_0_OFFSET, TOP_U_DDP_1_OFFSET, TOP_U_DDP_2_OFFSET, TOP_U_DDP_3_OFFSET};
    for (uint32_t ddp = 0; ddp < ddp_num; ++ddp) {
        phal_block_ctx_enter(phal_ctx, ddp_reg_offset[ddp]);
        status = phal_read(phal_ctx, DDP_U_DDP_ASC_HASH_REG_0_OFFSET + DDP_ASC_HASH_REG_DMEM_SIZE_OFFSET, &data);
        // Probe: same register block, reset default 0x55d2061. If this also
        // reads 0, the ASC_HASH_REG read path is dead and DMEM_SIZE=0 is
        // not trustworthy; if it reads back the default, DMEM_SIZE is really 0.
        uint32_t lut0 = 0;
        status |= phal_read(phal_ctx, DDP_U_DDP_ASC_HASH_REG_0_OFFSET + DDP_ASC_HASH_REG_LUT_000_OFFSET, &lut0);
        uint32_t addr_dir = 0;
        status |= phal_read(phal_ctx, DDP_U_DDP_ASC_HASH_REG_0_OFFSET + DDP_ASC_HASH_REG_ASC_ADDR_DIR_OFFSET, &addr_dir);

        status |= phal_field_write(phal_ctx, DDP_U_DDP_NDM_REG_0_OFFSET + DDP_NDM_REG_NDM_CTRL_OFFSET, DDP_NDM_REG_NDM_CTRL_QMS_Q_PHASE_bm, DDP_NDM_REG_NDM_CTRL_QMS_Q_PHASE_bp, 0x0 + ddp);
        status |= phal_field_write(phal_ctx, DDP_U_DDP_NDM_REG_0_OFFSET + DDP_NDM_REG_NDM_CTRL_OFFSET, DDP_NDM_REG_NDM_CTRL_QMS_STATION_ID_bm, DDP_NDM_REG_NDM_CTRL_QMS_STATION_ID_bp, 0xc + ddp);
        status |= phal_read(phal_ctx, DDP_U_DDP_NDM_REG_0_OFFSET + DDP_NDM_REG_NDM_CTRL_OFFSET, &ndm_ctrl);
        phal_block_ctx_exit(phal_ctx, ddp_reg_offset[ddp]);
        ti.logger->info("U_DDP_" + std::to_string(ddp) + " DMEM_SIZE=" + hex(data) + " NDM_CTRL=" + hex(ndm_ctrl) + " LUT0=" + hex(lut0) + " ADDR_DIR=" + hex(addr_dir));
    }

    if (status != PHAL_STATUS_OK) return status;

    // Demonstrate host-controlled SBR after all endpoint BAR accesses. The
    // helper changes only the upstream bridge's standard reset bit; the caller
    // owns assertion and post-deassertion delays.
    constexpr uint32_t SBR_ASSERT_DELAY_US = 100000;
    constexpr uint32_t SBR_RECOVERY_DELAY_US = 100000;
    status = ti.hal->secondary_bus_reset(true);
    if (status != PHAL_STATUS_OK) return status;
    phal_delay_us(ti.phal_ctx, SBR_ASSERT_DELAY_US);

    status = ti.hal->secondary_bus_reset(false);
    if (status != PHAL_STATUS_OK) return status;
    phal_delay_us(ti.phal_ctx, SBR_RECOVERY_DELAY_US);
    return PHAL_STATUS_OK;
}
