#include "diag/modules/PMUModule.h"

PMUModule::PMUModule(const std::string& name, const DeviceContext& ctx,
                     const ModuleInstanceConfig& config)
    : TestTarget(name, "pmu", ctx)
{
    ctx_.module_index = config.index;
    _add_test("example", {},
              [this](TestInfo& ti) { return example(ti); });
    _add_test("uart_example", {},
              [this](TestInfo& ti) { return uart_example(ti); });
    _add_test("uart_read32_example", {{"addr", "", "u64"}},
              [this](TestInfo& ti) { return uart_read32_example(ti); });
    _add_test("uart_write32_example",
              {{"addr", "", "u64"}, {"value", "", "u32"}},
              [this](TestInfo& ti) { return uart_write32_example(ti); });
}
