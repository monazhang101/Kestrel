#pragma once

#include "diag/core/TestTarget.h"

class PMUModule : public TestTarget {
private:
    TestStatus example(TestInfo& ti);

public:
    PMUModule(const std::string& name, const DeviceContext& ctx,
                const ModuleInstanceConfig& config);

};
