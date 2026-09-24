#include "diag/modules/ISIModule.h"
#include "diag/core/Common.h"
#include "diag/core/PhalBridge.h"

using common::format::hex;

extern "C" {
#include <phal/components/isi/isi.h>
#if __has_include(<phal/ops.h>)
#include <phal/ops.h>
#endif
}

TestStatus ISIModule::example(TestInfo& ti)
{
    auto& ctx = ctx_;
    uint32_t data = 0;

    // This ISI target's base + 0x1c; no block offset.
    auto status = phal_read(ctx.phal, 0x1c, &data);
    if (status != PHAL_STATUS_OK) return status;
    if (data != 0x202020) {
        ti.logger->error("ISI + 0x1c: expected 0x202020, actual=" + hex(data, 8));
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

    // 3. Native PHAL entry: framework initialized this ISI module's root ctx.
    const auto native = phal_component_isi_linkup(ctx.phal, 1000000);
    if (native != PHAL_STATUS_OK)
        ti.logger->error("phal_component_isi_linkup failed: status=" + std::to_string(static_cast<int>(native)));
    status |= native;

    return status;
}
