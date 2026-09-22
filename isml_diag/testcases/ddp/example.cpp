#include "diag/modules/DDPModule.h"
#include "diag/core/Common.h"
#include "diag/core/PhalBridge.h"

extern "C" {
#include <phal/phal.h>
#if __has_include(<phal/ops.h>)
#include <phal/ops.h>
#endif
}

using common::format::hex;

TestStatus DDPModule::example(TestInfo& ti)
{
    auto& ctx = ctx_;
    uint32_t data = 0;

    // DMC0 DID/VID: internal 0x12080800, this DDP target's BAR0 base + 0x80800.
    phal_block_ctx_enter(ctx.phal, 0x80800);
    auto status = phal_read(ctx.phal, 0x0, &data);
    phal_block_ctx_exit(ctx.phal, 0x80800);
    if (status != PHAL_STATUS_OK) return status;
    if (data != 0xabcd16c3) {
        ti.logger->error("DMC0 DID/VID: expected 0xabcd16c3, actual=" + hex(data, 8));
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
