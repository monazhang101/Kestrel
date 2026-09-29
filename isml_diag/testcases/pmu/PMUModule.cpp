#include "diag/modules/PMUModule.h"

PMUModule::PMUModule(const std::string& name, const DeviceContext& ctx,
                         const ModuleInstanceConfig& config)
    : TestTarget(name, "pmu", ctx)
{
    ctx_.module_index = config.index;
    _add_test("example", {},
              [this](TestInfo& ti) { return example(ti); });
}
