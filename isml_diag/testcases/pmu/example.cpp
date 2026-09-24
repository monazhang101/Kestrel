#include "diag/modules/PMUModule.h"
#include "diag/core/Common.h"
#include "diag/core/PhalBridge.h"

extern "C" {
#include <phal/phal.h>
#if __has_include(<phal/ops.h>)
#include <phal/ops.h>
#endif
}

#define PMU_PLL_REG 0x2100000U
#define PMU_REG_OFFSET 0x800U
#define PMU_UNLOCK_VALUE 0x5A5A5A5AU

using common::format::hex;

TestStatus PMUModule::example(TestInfo& ti)
{
    auto& ctx = ctx_;
    uint32_t data = 0;

    // Unlock host access at the PMU base before reading PMU registers.
    auto status = phal_write(ctx.phal, 0x0, PMU_UNLOCK_VALUE);
    if (status != PHAL_STATUS_OK) return status;

    // PLL register: internal PMU address 0x18000000 + 0x2100000 + 0x800.
    phal_block_ctx_enter(ctx.phal, PMU_PLL_REG);
    status = phal_read(ctx.phal, PMU_REG_OFFSET, &data);
    phal_block_ctx_exit(ctx.phal, PMU_PLL_REG);
    if (status != PHAL_STATUS_OK) return status;
    if (data != 0x141e01) {
        ti.logger->error("PMU PLL register: expected 0x00141e01, actual=" + hex(data, 8));
        return PHAL_STATUS_ERROR;
    }

    // Call other module PCIE
    auto& pcie = ti.module("pcie", 0);
    auto hqc_sram = mem_alloc(pcie, HQC_SRAM_0, 4);
    if (!hqc_sram.valid()) return PHAL_STATUS_ERROR;
    status = mem_write(hqc_sram, 0, 0x23232323);
    status |= mem_read(hqc_sram, 0, &data);
    if (data != 0x23232323)
        return PHAL_STATUS_ERROR;

    // Allocate a private word; its offset is chosen by the framework.
    auto dmem = mem_alloc(ctx, DMEM, 4);
    if (!dmem.valid()) return PHAL_STATUS_ERROR;
    status |= mem_read(dmem, 0x0, &data);
    if (status == PHAL_STATUS_OK) {
        uint32_t actual = 0;
        status |= mem_write(dmem, 0x0, 0x12345678);
        status |= mem_read(dmem, 0x0, &actual);
        status |= mem_write(dmem, 0x0, data);
        if (actual != 0x12345678) status |= PHAL_STATUS_ERROR;
    }
    return status;
}
