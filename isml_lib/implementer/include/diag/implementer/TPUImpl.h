#pragma once

#include "diag/core/TestInfo.h"

class TPUImpl {
public:
    virtual ~TPUImpl() = default;

    virtual TestResult Identify(TestInfo& ti) = 0;
};
