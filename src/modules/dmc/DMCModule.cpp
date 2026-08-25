#include "diag/module/DMCModule.h"

#include <utility>

DMCModule::DMCModule(const std::string& name,
                     const DeviceContext& ctx,
                     uint32_t ddp_id,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<DMCImpl> impl)
    : BaseDevice(name, ctx),
      ddp_id_(ddp_id),
      controller_id_(config.index),
      reg_offset_(config.reg_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("dmc_status_check", [this](TestInfo& ti) { return DmcStatusCheck(ti); });
    _add_test("dmc_reg_scan", [this](TestInfo& ti) { return DmcRegScan(ti); });
}
