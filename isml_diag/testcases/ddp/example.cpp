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
    auto& ddp0 = ctx_;
    auto& ddp1 = ti.module("ddp", 1);
    // Other instances use the same lookup:
    // auto& ddp2 = ti.module("ddp", 2);
    // auto& ddp3 = ti.module("ddp", 3);
    uint32_t data = 0;

    // DMC0 DID/VID: internal 0x12080800, this DDP target's BAR0 base + 0x80800.
    phal_block_ctx_enter(ddp0.phal, 0x80800);
    auto status = phal_read(ddp0.phal, 0x0, &data);
    phal_block_ctx_exit(ddp0.phal, 0x80800);
    if (status != PHAL_STATUS_OK) return status;
    if (data != 0xabcd16c3) {
        ti.logger->error("DMC0 DID/VID: expected 0xabcd16c3, actual=" + hex(data, 8));
        return PHAL_STATUS_ERROR;
    }

    // DMC0 DID/VID: internal 0x13080800, this DDP target's BAR0 base + 0x80800.
    phal_block_ctx_enter(ddp1.phal, 0x80800);
    status = phal_read(ddp1.phal, 0x0, &data);
    phal_block_ctx_exit(ddp1.phal, 0x80800);
    if (status != PHAL_STATUS_OK) return status;
    if (data != 0xabcd16c3) {
        ti.logger->error("DMC0 DID/VID: expected 0xabcd16c3, actual=" + hex(data, 8));
        return PHAL_STATUS_ERROR;
    }

    // Allocate a private word; its offset is chosen by the framework.
    auto dmem = mem_alloc(ddp0, DMEM, 4);
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
