#include "diag/modules/PCIeModule.h"
#include "diag/core/Common.h"
#include "diag/core/PhalBridge.h"

extern "C" {
#include <phal/phal.h>
#if __has_include(<phal/ops.h>)
#include <phal/ops.h>
#endif
#include <atlas_csr_sw/src/h/TOP.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/PCIE.h>
#include <atlas_csr_sw/src/h/TOP_deps/PCIE/PCIE_APP_REG_FOR_AXICLK/PCIE_APP_REG_FOR_AXICLK.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP_ASC_HASH_REG/DDP_ASC_HASH_REG.h>
#include <atlas_csr_sw/src/h/TOP_deps/DDP/DDP_NDM_REG/DDP_NDM_REG.h>
}

#include <array>

using common::format::hex;

TestStatus PCIeModule::example(TestInfo& ti)
{
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
    auto status = phal_read(phal_ctx, 0x1000000, &data);
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
        status |= phal_field_write(phal_ctx, DDP_U_DDP_NDM_REG_0_OFFSET + DDP_NDM_REG_NDM_CTRL_OFFSET, DDP_NDM_REG_NDM_CTRL_QMS_Q_STATION_ID_bm, DDP_NDM_REG_NDM_CTRL_QMS_Q_STATION_ID_bp, 0xc + ddp);
        status |= phal_read(phal_ctx, DDP_U_DDP_NDM_REG_0_OFFSET + DDP_NDM_REG_NDM_CTRL_OFFSET, &ndm_ctrl);
        phal_block_ctx_exit(phal_ctx, ddp_reg_offset[ddp]);
        ti.logger->info("U_DDP_" + std::to_string(ddp) + " DMEM_SIZE=" + hex(data) + " NDM_CTRL=" + hex(ndm_ctrl) + " LUT0=" + hex(lut0) + " ADDR_DIR=" + hex(addr_dir));
    }

    if (status != PHAL_STATUS_OK) return status;

    std::ostringstream message;
    uint32_t rd_data = 0;
    const uint32_t dmc_valid[] = {1, 1, 1};
    uint64_t dmc_cap = 0x100000000 * 8; // 4G * 8 = 32G

    // test harvesting
    // Visit the head and tail of every valid DMC address space: write then
    // read back, with a value that identifies ddp/dmc/head-or-tail.
    for (uint32_t ddp = 0; ddp < ddp_num; ++ddp) {
        for (uint32_t dmc = 0; dmc < sizeof(dmc_valid) / sizeof(dmc_valid[0]); ++dmc) {
            if (!dmc_valid[dmc]) continue;
            const uint64_t dmc_base = dmc_cap * (ddp * 3 + dmc);
            const uint64_t edges[] = {0x0, dmc_cap / 2 - 4}; // head / tail (word aligned)
            for (uint32_t edge = 0; edge < 2; ++edge) {
                const uint32_t pattern = (ddp << 24) | (dmc << 20) |
                                         (edge ? 0xE : 0x0) | 0x100;
                auto word = ti.hal->device_mem_alloc(
                    DMEM, sizeof(uint32_t), dmc_base + edges[edge]);
                if (!word.valid()) {
                    status |= PHAL_STATUS_ERROR;
                    continue;
                }
                auto op = mem_write(word, 0, pattern);
                op |= mem_read(word, 0, &rd_data);
                if (op == PHAL_STATUS_OK && rd_data != pattern) {
                    ti.logger->error("harvest mismatch: ddp=" + std::to_string(ddp) +
                                     " dmc=" + std::to_string(dmc) +
                                     " offset=" + hex(dmc_base + edges[edge]) +
                                     " expected=" + hex(pattern, 8) +
                                     " actual=" + hex(rd_data, 8));
                    op |= PHAL_STATUS_ERROR;
                }
                status |= op;
                message.str("");
                message << "harvest: ddp=" << ddp << " dmc=" << dmc
                        << " edge=" << (edge ? "tail" : "head")
                        << " offset: " << hex(dmc_base + edges[edge])
                        << " data: " << hex(rd_data)
                        << " status: " << op;
                ti.logger->info(message.str());
            }
        }
    }

    if (status != PHAL_STATUS_OK) return status;
    return status;
}
