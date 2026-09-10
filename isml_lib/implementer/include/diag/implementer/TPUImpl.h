#pragma once

#include "diag/core/TestInfo.h"

class TPUImpl {
public:
    virtual ~TPUImpl() = default;

    virtual TestStatus Identify(TestInfo& ti) = 0;
};
