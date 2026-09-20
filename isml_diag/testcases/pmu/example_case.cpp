#include "diag/modules/PMUModule.h"
#include "diag/core/Common.h"

// Set write_enable=1 only for a confirmed ordinary read/write scratch register
// and DMEM region. W1C, read-clear and command registers cannot use this restore.
TestStatus PMUModule::example(TestInfo& ti)
{
    auto& ctx = ctx_;
    const auto block = common::args::get_u64(ti.args, "block_offset");
    const auto reg_offset = common::args::get_u64(ti.args, "reg_offset");
    const auto mem_offset = common::args::get_u64(ti.args, "dmem_offset");
    const auto value = static_cast<uint32_t>(common::args::get_u64(ti.args, "value"));
    const bool write_enable = common::args::get_u64(ti.args, "write_enable") != 0;

    // 1. Module base + block + register offset. PHAL env.base is unchanged.
    auto status = block_ctx_enter(ctx, block);
    if (status != TestStatus::OK) return status;
    uint32_t original = 0;
    auto step = reg_read(ctx, reg_offset, &original);
    status |= step;
    if (step == TestStatus::OK && write_enable) {
        status |= reg_write(ctx, reg_offset, value);
        uint32_t actual = 0;
        step = reg_read(ctx, reg_offset, &actual);
        status |= step;
        if (step == TestStatus::OK && actual != value) {
            ti.logger->error("register readback mismatch");
            status |= TestStatus::ERROR;
        }
        status |= reg_write(ctx, reg_offset, original);
    }
    status |= block_ctx_exit(ctx, block);
    if (status != TestStatus::OK) return status;

    // 2. DMEM base + byte offset; aperture setup is hidden in the three-arg API.
    step = dmem_read(ctx, mem_offset, &original);
    status |= step;
    if (step == TestStatus::OK && write_enable) {
        status |= dmem_write(ctx, mem_offset, value);
        uint32_t actual = 0;
        step = dmem_read(ctx, mem_offset, &actual);
        status |= step;
        if (step == TestStatus::OK && actual != value) {
            ti.logger->error("D-MEM readback mismatch");
            status |= TestStatus::ERROR;
        }
        status |= dmem_write(ctx, mem_offset, original);
    }
    if (status != TestStatus::OK) return status;

    return status;
}
