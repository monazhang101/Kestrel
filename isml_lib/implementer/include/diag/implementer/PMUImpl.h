#pragma once

#include "diag/core/TestInfo.h"

class PMUImpl {
public:
    virtual ~PMUImpl() = default;

    virtual TestStatus PmuIpcRequestStart(TestInfo& ti) = 0;
    virtual TestStatus PmuIpcRequestExec(TestInfo& ti) = 0;
    virtual TestStatus PmuIpcRequestFinish(TestInfo& ti) = 0;
    virtual TestStatus PmuRegRead(TestInfo& ti) = 0;
    virtual TestStatus PmuRegWrite(TestInfo& ti) = 0;
    virtual TestStatus PmuRegCheck(TestInfo& ti) = 0;
};
