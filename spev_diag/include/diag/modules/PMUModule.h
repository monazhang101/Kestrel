#pragma once

#include "diag/core/TestTarget.h"

class PMUModule : public TestTarget {
private:
    TestStatus example(TestInfo& ti);
    TestStatus uart_example(TestInfo& ti);
    TestStatus uart_read32_example(TestInfo& ti);
    TestStatus uart_write32_example(TestInfo& ti);

public:
    PMUModule(const std::string& name, const DeviceContext& ctx,
                const ModuleInstanceConfig& config);

};
