#pragma once

#include "diag/core/TestInfo.h"

class ISIImpl {
public:
    virtual ~ISIImpl() = default;

    virtual TestStatus IsiLinkup(TestInfo& ti) = 0;
    virtual TestStatus IsiSetup(TestInfo& ti) = 0;
};
