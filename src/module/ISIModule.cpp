#include "diag/module/ISIModule.h"

#include <utility>

ISIModule::ISIModule(const std::string& name,
                     const DeviceContext& ctx,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<ISIImpl> impl)
    : BaseDevice(name, ctx),
      link_id_(config.index),
      reg_offset_(config.reg_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
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
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "ISI implementation is not bound");
    }
    return impl_->IsiLinkup(ti);
}

// isi_setup : To initialize ISI link configuration for this ISI instance.
// @input: args["mode"] resolved by YAML defaults.
// @output: TestResult metrics include link_id, mode, and setup_status.
TestResult ISIModule::IsiSetup(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "ISI implementation is not bound");
    }
    return impl_->IsiSetup(ti);
}
