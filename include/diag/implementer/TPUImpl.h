#pragma once

#include "diag/core/TestInfo.h"

class TPUImpl {
public:
    virtual ~TPUImpl() = default;

    virtual TestResult Identify(TestInfo& ti) = 0;
    virtual TestResult SocGpioDirSet(TestInfo& ti) = 0;
    virtual TestResult SocGpioRead(TestInfo& ti) = 0;
    virtual TestResult SocGpioWrite(TestInfo& ti) = 0;
};
