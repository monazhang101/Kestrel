#include "diag/modules/ISIModule.h"
#include "diag/core/Common.h"
#include "diag/core/HalContext.h"

TestStatus ISIModule::example(TestInfo& ti)
{
    uint32_t data = 0;

    auto status = PHAL_STATUS_OK;

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
    if (status != PHAL_STATUS_OK) return status;

    return status;
}
