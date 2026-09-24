#include "diag/modules/PCIeModule.h"
#include "diag/core/Common.h"
#include "diag/core/PhalBridge.h"

extern "C" {
#include <phal/phal.h>
#if __has_include(<phal/ops.h>)
#include <phal/ops.h>
#endif
}

using common::format::hex;

TestStatus PCIeModule::example(TestInfo& ti)
{
    auto& ctx = ctx_;
    uint32_t data = 0;

    // PCIe DID/VID: this PCIe target's BAR0 base + 0x100000 + 0x0.
    phal_block_ctx_enter(ctx.phal, 0x100000);
    auto status = phal_read(ctx.phal, 0x0, &data);
    phal_block_ctx_exit(ctx.phal, 0x100000);
    if (status != PHAL_STATUS_OK) return status;
    if (data != 0xabcd16c3) {
        ti.logger->error("PCIe DID/VID: expected 0xabcd16c3, actual=" + hex(data, 8));
        return PHAL_STATUS_ERROR;
    }

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
    if (status != PHAL_STATUS_OK) return status;

    // The same allocation API handles module-local RCF memory.
    auto hqc_sram = mem_alloc(ctx, HQC_SRAM_0, 4);
    if (!hqc_sram.valid()) return PHAL_STATUS_ERROR;
    status = mem_read(hqc_sram, 0x0, &data);
    if (status == PHAL_STATUS_OK) {
        uint32_t actual = 0;
        status |= mem_write(hqc_sram, 0x0, 0x87654321);
        status |= mem_read(hqc_sram, 0x0, &actual);
        status |= mem_write(hqc_sram, 0x0, data);
        if (actual != 0x87654321) status |= PHAL_STATUS_ERROR;
    }
    return status;
}
