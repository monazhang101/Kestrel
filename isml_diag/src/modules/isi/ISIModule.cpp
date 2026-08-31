#include "diag/module/ISIModule.h"

#include "diag/core/Common.h"

#include <utility>

ISIModule::ISIModule(const std::string& name,
                     const DeviceContext& ctx,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<ISIImpl> impl)
    : BaseDevice(name, ctx),
      link_id_(config.index),
      bar_index_(config.bar_index),
      reg_offset_(config.reg_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    auto* bar_base = static_cast<uint8_t*>(common::bar::mapped_base(ctx_, bar_index_));
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("isi_linkup", [this](TestInfo& ti) { return IsiLinkup(ti); });
    _add_test("isi_setup", [this](TestInfo& ti) { return IsiSetup(ti); });
}
