#pragma once

#include "diag/core/TestInfo.h"

class DMCImpl {
public:
    virtual ~DMCImpl() = default;

    virtual TestStatus status_check(TestInfo& ti) = 0;
    virtual TestStatus reg_scan(TestInfo& ti) = 0;
};
