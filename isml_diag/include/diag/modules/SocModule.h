#pragma once

#include "diag/core/TestTarget.h"

// Composite tests share their parent's modules; SoC has no register base.
class SocModule : public TestTarget {
    TestStatus example(TestInfo& ti);
public:
    SocModule(const std::string& name, const DeviceContext& ctx);
};
