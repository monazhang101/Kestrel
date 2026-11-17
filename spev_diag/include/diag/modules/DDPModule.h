#pragma once

#include "diag/core/TestTarget.h"

class DDPModule : public TestTarget {
private:
    TestStatus example(TestInfo& ti);

public:
    DDPModule(const std::string& name, const DeviceContext& ctx,
                const ModuleInstanceConfig& config);

};
