#include "diag/modules/PMUModule.h"
#include "diag/core/Common.h"
#include "diag/core/Format.h"
#include "diag/core/HalContext.h"

#include <atlas_csr_sw/src/h/TOP.h>
#include <limits>
#include <stdexcept>

using common::format::hex;

namespace {
// PMU-local absolute byte addresses. PCIe accesses the PMU through its RCF
// window (PMU core + 0x800 DB window); PCIe ctx supplies the chip RCF base.
// Address adaptation belongs to this testcase, not to the UART framework.
constexpr uintptr_t PMU_READY_ADDR = 0x0210205cu;
constexpr uint32_t PMU_READY_VALUE = 0x5b5b5b5bu;
constexpr uintptr_t PMU_DIAG_REG0_ADDR = 0x02105060u;
constexpr uintptr_t PMU_RCF_WINDOW = 0x800u;

uintptr_t register_address(const TestInfo& ti, uint64_t pmu_address)
{
    const uint64_t offset = ti.transport == RegisterTransport::Uart ? 0 :
        uint64_t(TOP_U_PMU_CORE_0_OFFSET) + PMU_RCF_WINDOW;
    if (pmu_address > std::numeric_limits<uintptr_t>::max() - offset)
        throw std::invalid_argument("PMU address is outside the host address range");
    return uintptr_t(pmu_address + offset);
}
}

TestStatus PMUModule::example(TestInfo& ti)
{
    uint32_t data = 0;
    auto status = PHAL_STATUS_OK;

    // Allocate a private word; its offset is chosen by the framework.
    auto dmem = ti.hal->device_mem_alloc(DMEM, 4);
    if (!dmem.valid()) return PHAL_STATUS_ERROR;
    ti.logger->info("PMU example DMEM allocation: offset=" + hex(dmem.offset()) +
                    " region_base=" + hex(dmem.region_base()) +
                    " absolute_addr=" + hex(dmem.address()) +
                    " size_bytes=" + std::to_string(dmem.size()));
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

TestStatus PMUModule::uart_example(TestInfo& ti)
{
    uint32_t ready = 0;
    auto status = phal_read(ti.phal_ctx, register_address(ti, PMU_READY_ADDR), &ready);
    if (status != PHAL_STATUS_OK) return status;
    ti.logger->info("PMU ready=" + hex(ready, 8));
    if (ready != PMU_READY_VALUE) {
        ti.logger->error("PMU ready mismatch: expected=" + hex(PMU_READY_VALUE, 8));
        return PHAL_STATUS_ERROR;
    }
    constexpr uint32_t EXPECTED = 0x12345678;
    const auto addr = register_address(ti, PMU_DIAG_REG0_ADDR);
    status = phal_write(ti.phal_ctx, addr, EXPECTED);
    if (status != PHAL_STATUS_OK) return status;
    uint32_t actual = 0;
    status = phal_read(ti.phal_ctx, addr, &actual);
    if (status != PHAL_STATUS_OK) return status;
    ti.logger->info("PMU REG0 read-back=" + hex(actual, 8) + " expected=" + hex(EXPECTED, 8));
    return actual == EXPECTED ? PHAL_STATUS_OK : PHAL_STATUS_ERROR;
}

TestStatus PMUModule::uart_read32_example(TestInfo& ti)
{
    // Despite its UART-focused name, this case uses the executable-selected
    // backend; the case owns conversion from the PMU-local input address.
    const auto address = common::args::get_u64(ti.args, "addr");
    if (address % 4) return PHAL_STATUS_INVALID;
    uint32_t value = 0;
    const auto status = phal_read(ti.phal_ctx, register_address(ti, address), &value);
    if (status == PHAL_STATUS_OK)
        ti.logger->info("PMU read addr=" + hex(address) + " value=" + hex(value, 8));
    return status;
}

TestStatus PMUModule::uart_write32_example(TestInfo& ti)
{
    const auto address = common::args::get_u64(ti.args, "addr");
    if (address % 4) return PHAL_STATUS_INVALID;
    const auto value = uint32_t(common::args::get_u64(ti.args, "value"));
    const auto status = phal_write(ti.phal_ctx, register_address(ti, address), value);
    if (status == PHAL_STATUS_OK)
        ti.logger->info("PMU write addr=" + hex(address) + " value=" + hex(value, 8));
    return status;
}
