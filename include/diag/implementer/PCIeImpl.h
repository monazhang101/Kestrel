#pragma once

#include "diag/core/TestInfo.h"

class PCIeImpl {
public:
    virtual ~PCIeImpl() = default;

    virtual TestResult PcieLinkStatusGet(TestInfo& ti) = 0;
    virtual TestResult PcieDmaDataTransfer(TestInfo& ti) = 0;
};
