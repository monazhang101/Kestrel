#include "diag/modules/DDPModule.h"

DDPModule::DDPModule(const std::string& name, const DeviceContext& ctx,
                         const ModuleInstanceConfig& config)
    : TestTarget(name, "ddp", ctx)
{
    ctx_.module_index = config.index;
    _add_test("example", {},
              [this](TestInfo& ti) { return example(ti); });
}
