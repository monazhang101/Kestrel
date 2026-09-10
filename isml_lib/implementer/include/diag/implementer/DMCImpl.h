#pragma once

#include "diag/core/TestInfo.h"

class DMCImpl {
public:
    virtual ~DMCImpl() = default;

    virtual TestStatus DmcStatusCheck(TestInfo& ti) = 0;
    virtual TestStatus DmcRegScan(TestInfo& ti) = 0;
};
