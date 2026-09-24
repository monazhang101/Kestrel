#include "diag/modules/SocModule.h"
#include "diag/core/Common.h"

TestStatus SocModule::example(TestInfo& ti)
{
    auto& ddp0 = ti.module("ddp", 0);
    auto& ddp1 = ti.module("ddp", 1);
    auto& pcie = ti.module("pcie", 0);
    // Separate DDP pools may use the same relative offset. HQC belongs to PCIe.
    auto first = mem_alloc(ddp0, DDP_ILM, 4);
    auto second = mem_alloc(ddp1, DDP_ILM, 4);
    auto hqc = mem_alloc(pcie, HQC_SRAM_0, 4);
    if (!first.valid() || !second.valid() || !hqc.valid()) return PHAL_STATUS_ERROR;
    auto status = mem_write(first, 0, 0x11223344);
    status |= mem_write(second, 0, 0x55667788);
    status |= mem_write(hqc, 0, 0xaabbccdd);
    if (status != PHAL_STATUS_OK) return status;
    uint32_t a = 0, b = 0, c = 0;
    status |= mem_read(first, 0, &a);
    status |= mem_read(second, 0, &b);
    status |= mem_read(hqc, 0, &c);
    if (a != 0x11223344 || b != 0x55667788 || c != 0xaabbccdd)
        status |= PHAL_STATUS_ERROR;
    return status;
}
