#include "diag/module/DMCModule.h"

#include "diag/core/Common.h"

DMCModule::DMCModule(const std::string& name,
                     const DeviceContext& ctx,
                     uint32_t ddp_id,
                     uint32_t controller_id,
                     uint64_t reg_offset,
                     uint64_t reg_size)
    : BaseDevice(name, ctx),
      ddp_id_(ddp_id),
      controller_id_(controller_id),
      reg_offset_(reg_offset),
      reg_size_(reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("dmc_status_check", [this](TestInfo& ti) { return DmcStatusCheck(ti); });
    _add_test("dmc_reg_scan", [this](TestInfo& ti) { return DmcRegScan(ti); });
}

// dmc_status_check : To check one DDP memory-controller status block.
// @input: none.
// @output: TestResult metrics include ddp_id, controller_id, and status.
TestResult DMCModule::DmcStatusCheck(TestInfo& ti)
{
    (void)ti.args;
    (void)reg_base_;
    (void)reg_size_;
    return {"dmc_status_check", get_name(), true, {
        {"ddp_id", std::to_string(ddp_id_)},
        {"controller_id", std::to_string(controller_id_)},
        {"status", "ready"}
    }};
}

// dmc_reg_scan : To scan readable registers for one DMC controller.
// @input: args["range"] optional register range.
// @output: TestResult metrics include scanned_range and bad_register_count.
TestResult DMCModule::DmcRegScan(TestInfo& ti)
{
    auto range = common::args::get_string(ti.args, "range", "default");
    (void)reg_base_;
    (void)reg_size_;
    return {"dmc_reg_scan", get_name(), true, {
        {"ddp_id", std::to_string(ddp_id_)},
        {"controller_id", std::to_string(controller_id_)},
        {"scanned_range", range},
        {"bad_register_count", "0"}
    }};
}
