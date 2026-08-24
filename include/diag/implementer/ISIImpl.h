#pragma once

#include "diag/core/TestInfo.h"

class ISIImpl {
public:
    virtual ~ISIImpl() = default;

    virtual TestResult IsiLinkup(TestInfo& ti) = 0;
    virtual TestResult IsiSetup(TestInfo& ti) = 0;
};
