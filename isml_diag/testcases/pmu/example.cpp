#include "diag/modules/PMUModule.h"
#include "diag/core/Common.h"
#include "diag/core/HalContext.h"

TestStatus PMUModule::example(TestInfo& ti)
{
    uint32_t data = 0;

    auto status = PHAL_STATUS_OK;

    auto hqc_sram = ti.hal->device_mem_alloc(HQC_SRAM_0, 4);
    if (!hqc_sram.valid()) return PHAL_STATUS_ERROR;
    status = mem_write(hqc_sram, 0, 0x23232323);
    status |= mem_read(hqc_sram, 0, &data);
    if (data != 0x23232323)
        return PHAL_STATUS_ERROR;

    // Allocate a private word; its offset is chosen by the framework.
    auto dmem = ti.hal->device_mem_alloc(DMEM, 4);
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
