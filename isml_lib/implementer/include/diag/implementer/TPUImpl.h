#pragma once

#include "diag/core/TestInfo.h"

class TPUImpl {
public:
    virtual ~TPUImpl() = default;

    virtual TestStatus identify(TestInfo& ti) = 0;
};
