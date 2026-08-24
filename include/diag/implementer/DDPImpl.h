#pragma once

#include "diag/core/TestInfo.h"

class DDPImpl {
public:
    virtual ~DDPImpl() = default;

    virtual TestResult DdpDmemLinkupVerify(TestInfo& ti) = 0;
};
