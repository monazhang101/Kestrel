#include "diag/modules/SocModule.h"

SocModule::SocModule(const std::string& name, const DeviceContext& ctx)
    : TestTarget(name, TargetType::Soc, ctx)
{
    _add_test("example", {}, [this](TestInfo& ti) { return example(ti); });
}
