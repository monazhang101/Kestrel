#include "diag/modules/ISIModule.h"

ISIModule::ISIModule(const std::string& name, const DeviceContext& ctx,
                         const ModuleInstanceConfig& config)
    : TestTarget(name, TargetType::Isi, ctx)
{
    ctx_.module_index = config.index;
    _add_test("example", {},
              [this](TestInfo& ti) { return example(ti); });
}
