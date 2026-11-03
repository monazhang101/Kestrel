#include "diag/modules/DDPModule.h"
#include "diag/core/Common.h"
#include "diag/core/HalContext.h"

TestStatus DDPModule::example(TestInfo& ti)
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

    // Native 64-bit aperture payload access is DMEM-only. Non-DMEM regions
    // are rejected by mem_read64()/mem_write64().
    auto dmem64 = ti.hal->device_mem_alloc(DMEM, 16, std::nullopt, 8);
    if (!dmem64.valid()) return status | PHAL_STATUS_ERROR;

    uint64_t original64 = 0;
    const auto saved64 = mem_read64(dmem64, 0x0, &original64);
    status |= saved64;
    if (saved64 == PHAL_STATUS_OK) {
        constexpr uint64_t EXPECTED64 = UINT64_C(0x1122334455667788);
        uint64_t actual64 = 0;
        status |= mem_write64(dmem64, 0x0, EXPECTED64);
        status |= mem_read64(dmem64, 0x0, &actual64);
        status |= mem_write64(dmem64, 0x0, original64);
        if (actual64 != EXPECTED64) status |= PHAL_STATUS_ERROR;
    }

    uint64_t misaligned = 0;
    if (mem_read64(dmem64, 0x4, &misaligned) != PHAL_STATUS_INVALID ||
        mem_write64(dmem64, 0x4, UINT64_C(0xaabbccddeeff0011)) !=
            PHAL_STATUS_INVALID) {
        status |= PHAL_STATUS_ERROR;
    }
    return status;
}
