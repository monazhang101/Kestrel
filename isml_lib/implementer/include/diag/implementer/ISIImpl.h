#pragma once

#include "diag/core/TestInfo.h"

class ISIImpl {
public:
    virtual ~ISIImpl() = default;

    virtual TestStatus linkup(TestInfo& ti) = 0;
    virtual TestStatus setup(TestInfo& ti) = 0;
};
