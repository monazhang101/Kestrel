#include "diag/modules/ISIModule.h"

#include <utility>

ISIModule::ISIModule(const std::string& name,
                     const DeviceContext& ctx,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<ISIImpl> impl)
    : TestTarget(name, "isi", ctx),
      link_id_(config.index),
      bar_index_(config.bar_index),
      reg_base_offset_(config.reg_base_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    _add_test("linkup", {}, [this](TestInfo& ti) { return linkup(ti); });
    _add_test("setup", {{"mode", "default", "string"}},
              [this](TestInfo& ti) { return setup(ti); });
}
