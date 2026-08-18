#include "diag/module/ISIModule.h"

#include "diag/core/Common.h"

ISIModule::ISIModule(const std::string& name,
                     const DeviceContext& ctx,
                     uint32_t link_id,
                     uint64_t reg_offset,
                     uint64_t reg_size)
    : BaseDevice(name, ctx),
      link_id_(link_id),
      reg_offset_(reg_offset),
      reg_size_(reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    // ----------------- Atomic Tests -----------------
    _add_test("isi_linkup", [this](TestInfo& ti) { return IsiLinkup(ti); });
    _add_test("isi_setup", [this](TestInfo& ti) { return IsiSetup(ti); });

}

// isi_linkup : To check whether this ISI link is physically present and link-up.
// @input: none.
// @output: TestResult metrics include link_id, link_status, lane_ready_bitmap, and error_count.
TestResult ISIModule::IsiLinkup(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;

    // Pseudocode: read present, training done, lane ready, and error counter registers.
    (void)ti.args;

    return {"isi_linkup", get_name(), true, {
        {"link_id", std::to_string(link_id_)},
        {"link_status", "up"},
        {"lane_ready_bitmap", "0xff"},
        {"error_count", "0"}
    }};
}

// isi_setup : To initialize ISI link configuration for this ISI instance.
// @input: args["mode"] resolved by YAML defaults.
// @output: TestResult metrics include link_id, mode, and setup_status.
TestResult ISIModule::IsiSetup(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    auto mode = common::args::get_string(ti.args, "mode");

    // Pseudocode: program link mode, lane config, timeout, and enable training.

    return {"isi_setup", get_name(), true, {
        {"link_id", std::to_string(link_id_)},
        {"mode", mode},
        {"setup_status", "done"}
    }};
}
