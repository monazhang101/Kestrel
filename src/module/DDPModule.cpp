#include "diag/module/DDPModule.h"

#include <utility>

DDPModule::DDPModule(const std::string& name,
                     const DeviceContext& ctx,
                     const DDPModuleConfig& config,
                     std::unique_ptr<DDPImpl> impl,
                     const std::shared_ptr<Implementer>& implementer)
    : BaseDevice(name, ctx),
      ddp_id_(config.index),
      reg_offset_(config.reg_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    // ----------------- Atomic Tests -----------------
    _add_test("ddp_dmem_linkup_verify", [this](TestInfo& ti) { return DdpDmemLinkupVerify(ti); });

    for (const auto& dmc_config : config.dmc_modules) {
        auto dmc_name = name + ".DMC_" + std::to_string(ddp_id_) + "_" +
                        std::to_string(dmc_config.index);
        auto* dmc_bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
        ModuleImplContext dmc_impl_ctx{
            dmc_name,
            ctx_,
            dmc_config.index,
            ddp_id_,
            dmc_bar_base == nullptr ? nullptr : dmc_bar_base + dmc_config.reg_offset,
            dmc_config.reg_offset,
            dmc_config.reg_size
        };

        std::unique_ptr<DMCImpl> dmc_impl;
        if (implementer != nullptr) {
            dmc_impl = implementer->dmc_ops(dmc_impl_ctx);
        }
        dmc_modules_.push_back(std::make_unique<DMCModule>(
            dmc_name,
            ctx_,
            ddp_id_,
            dmc_config,
            std::move(dmc_impl)));
    }
}

DMCModule* DDPModule::dmc(size_t index) const
{
    if (index >= dmc_modules_.size()) {
        return nullptr;
    }
    return dmc_modules_[index].get();
}

std::vector<BaseDevice*> DDPModule::child_targets() const
{
    std::vector<BaseDevice*> children;
    for (const auto& dmc_module : dmc_modules_) {
        children.push_back(dmc_module.get());
    }
    return children;
}


// ddp_dmem_linkup_verify : To verify DDP device-memory link-up state.
// @input: args["link"] resolved by YAML defaults.
// @output: TestResult metrics include link and linkup_status.
TestResult DDPModule::DdpDmemLinkupVerify(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "DDP implementation is not bound");
    }
    return impl_->DdpDmemLinkupVerify(ti);
}
