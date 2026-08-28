#pragma once

#include "diag/core/TestInfo.h"

class PMUImpl {
public:
    virtual ~PMUImpl() = default;

    virtual TestResult PmuIpcRequestStart(TestInfo& ti) = 0;
    virtual TestResult PmuIpcRequestExec(TestInfo& ti) = 0;
    virtual TestResult PmuIpcRequestFinish(TestInfo& ti) = 0;
    virtual TestResult PmuRegRead(TestInfo& ti) = 0;
    virtual TestResult PmuRegWrite(TestInfo& ti) = 0;
    virtual TestResult PmuRegCheck(TestInfo& ti) = 0;
};
