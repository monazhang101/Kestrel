#include "diag/modules/DMCModule.h"

#include <utility>

DMCModule::DMCModule(const std::string& name,
                     const DeviceContext& ctx,
                     uint32_t ddp_id,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<DMCImpl> impl)
    : TestTarget(name, "dmc", ctx),
      ddp_id_(ddp_id),
      controller_id_(config.index),
      bar_index_(config.bar_index),
      reg_base_offset_(config.reg_base_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    _add_test("status_check", {},
              [this](TestInfo& ti) { return status_check(ti); });
    _add_test("reg_scan", {{"range", "all", "string"}},
              [this](TestInfo& ti) { return reg_scan(ti); });
}
