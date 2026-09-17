#pragma once

#include "diag/core/TestInfo.h"

class DDPImpl {
public:
    virtual ~DDPImpl() = default;

    virtual TestStatus dmem_linkup_verify(TestInfo& ti) = 0;
};
