#include "diag/modules/PMUModule.h"

PMUModule::PMUModule(const std::string& name, const DeviceContext& ctx,
                         const ModuleInstanceConfig& config)
    : TestTarget(name, "pmu", ctx)
{
    ctx_.reg_base_offset = config.reg_base_offset;
    ctx_.reg_size = config.reg_size;
    _add_test("example", {},
              [this](TestInfo& ti) { return example(ti); });
}
