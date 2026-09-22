#include "diag/modules/PMUModule.h"
#include "diag/core/Common.h"
#include "diag/core/PhalBridge.h"

extern "C" {
#include <phal/phal.h>
#if __has_include(<phal/ops.h>)
#include <phal/ops.h>
#endif
}

#define EFUSE_CTRL_REG 0x2106000U

using common::format::hex;

TestStatus PMUModule::example(TestInfo& ti)
{
    auto& ctx = ctx_;
    uint32_t data = 0;

    // EFUSE CTRL revision: internal PMU base 0x18000000 + EFUSE_CTRL_REG.
    phal_block_ctx_enter(ctx.phal, EFUSE_CTRL_REG);
    auto status = phal_read(ctx.phal, 0x0, &data);
    phal_block_ctx_exit(ctx.phal, EFUSE_CTRL_REG);
    if (status != PHAL_STATUS_OK) return status;
    if (data != 0x1010001) {
        ti.logger->error("PMU EFUSE CTRL revision: expected 0x01010001, actual=" + hex(data, 8));
        return PHAL_STATUS_ERROR;
    }

    // DMEM offset 0: write a fixed pattern, then restore the original word.
    status |= dmem_read(ctx, 0x0, &data);
    if (status == PHAL_STATUS_OK) {
        uint32_t actual = 0;
        status |= dmem_write(ctx, 0x0, 0x12345678);
        status |= dmem_read(ctx, 0x0, &actual);
        status |= dmem_write(ctx, 0x0, data);
        if (actual != 0x12345678) status |= PHAL_STATUS_ERROR;
    }
    return status;
}
