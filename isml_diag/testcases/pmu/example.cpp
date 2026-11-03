#include "diag/modules/PMUModule.h"
#include "diag/core/Common.h"
#include "diag/core/Format.h"
#include "diag/core/HalContext.h"

#include <limits>

using common::format::hex;

namespace {
// Absolute PMU-local addresses forwarded unchanged by the UART PHAL context.
constexpr uintptr_t PMU_SYSTEM_READY_ADDR = 0x0210205cu;
constexpr uint32_t PMU_SYSTEM_READY_VALUE = 0x5b5b5b5bu;
constexpr uintptr_t PMU_DIAG_REG0_ADDR = 0x02105060u;
constexpr uint32_t PMU_DIAG_REG0_TEST_VALUE = 0x12345678u;
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
    if (status != PHAL_STATUS_OK) return status;

    auto* uart_ctx = ti.hal->uart_context();
    if (uart_ctx == nullptr) {
        ti.logger->error("PMU example UART context is not initialized");
        return PHAL_STATUS_ERROR;
    }

    uint32_t ready = 0;
    status = phal_read(uart_ctx, PMU_SYSTEM_READY_ADDR, &ready);
    if (status != PHAL_STATUS_OK) return status;
    ti.logger->info("PMU UART system ready: addr=" + hex(PMU_SYSTEM_READY_ADDR) +
                    " value=" + hex(ready, 8));
    if (ready != PMU_SYSTEM_READY_VALUE) {
        ti.logger->error("PMU UART system ready mismatch: expected=" +
                         hex(PMU_SYSTEM_READY_VALUE, 8) +
                         " actual=" + hex(ready, 8));
        return PHAL_STATUS_ERROR;
    }

    status = phal_write(uart_ctx, PMU_DIAG_REG0_ADDR,
                        PMU_DIAG_REG0_TEST_VALUE);
    if (status != PHAL_STATUS_OK) return status;

    uint32_t actual = 0;
    status = phal_read(uart_ctx, PMU_DIAG_REG0_ADDR, &actual);
    if (status != PHAL_STATUS_OK) return status;
    ti.logger->info("PMU UART REG0 read-back: addr=" + hex(PMU_DIAG_REG0_ADDR) +
                    " value=" + hex(actual, 8));
    if (actual != PMU_DIAG_REG0_TEST_VALUE) {
        ti.logger->error("PMU UART REG0 mismatch: expected=" +
                         hex(PMU_DIAG_REG0_TEST_VALUE, 8) +
                         " actual=" + hex(actual, 8));
        return PHAL_STATUS_ERROR;
    }
    return PHAL_STATUS_OK;
}

TestStatus PMUModule::uart_read32_example(TestInfo& ti)
{
    const auto address = common::args::get_u64(ti.args, "addr");
    if (address > std::numeric_limits<uintptr_t>::max()) return PHAL_STATUS_INVALID;
    uint32_t value = 0;
    const auto status = phal_read(ti.hal->uart_context(), uintptr_t(address), &value);
    if (status == PHAL_STATUS_OK)
        ti.logger->info("UART read addr=" + hex(address) + " value=" + hex(value, 8));
    return status;
}

TestStatus PMUModule::uart_write32_example(TestInfo& ti)
{
    const auto address = common::args::get_u64(ti.args, "addr");
    const auto value = uint32_t(common::args::get_u64(ti.args, "value"));
    if (address > std::numeric_limits<uintptr_t>::max()) return PHAL_STATUS_INVALID;
    const auto status = phal_write(ti.hal->uart_context(), uintptr_t(address), value);
    if (status == PHAL_STATUS_OK)
        ti.logger->info("UART write addr=" + hex(address) + " value=" + hex(value, 8));
    return status;
}
