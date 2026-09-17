#pragma once

#include "diag/core/TestInfo.h"

class PMUImpl {
public:
    virtual ~PMUImpl() = default;

    virtual TestStatus ipc_request_start(TestInfo& ti) = 0;
    virtual TestStatus ipc_request_exec(TestInfo& ti) = 0;
    virtual TestStatus ipc_request_finish(TestInfo& ti) = 0;
    virtual TestStatus reg_read(TestInfo& ti) = 0;
    virtual TestStatus reg_write(TestInfo& ti) = 0;
    virtual TestStatus reg_check(TestInfo& ti) = 0;
};
