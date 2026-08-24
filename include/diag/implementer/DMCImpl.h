#pragma once

#include "diag/core/TestInfo.h"

class DMCImpl {
public:
    virtual ~DMCImpl() = default;

    virtual TestResult DmcStatusCheck(TestInfo& ti) = 0;
    virtual TestResult DmcRegScan(TestInfo& ti) = 0;
};
