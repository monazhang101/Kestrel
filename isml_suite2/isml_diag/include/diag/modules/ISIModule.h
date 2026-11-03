#pragma once

#include "diag/core/TestTarget.h"

class ISIModule : public TestTarget {
private:
    TestStatus example(TestInfo& ti);

public:
    ISIModule(const std::string& name, const DeviceContext& ctx,
                const ModuleInstanceConfig& config);

};
