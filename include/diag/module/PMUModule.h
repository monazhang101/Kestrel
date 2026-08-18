#pragma once

#include "diag/core/BaseDevice.h"

#include <cstdint>
#include <string>

class PMUModule : public BaseDevice {
private:
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;

    TestResult PmuIpcRequestStart(TestInfo& ti);
    TestResult PmuIpcRequestExec(TestInfo& ti);
    TestResult PmuIpcRequestFinish(TestInfo& ti);
    TestResult PmuRegRead(TestInfo& ti);
    TestResult PmuRegWrite(TestInfo& ti);
    TestResult PmuRegCheck(TestInfo& ti);

public:
    PMUModule(const std::string& name,
              const DeviceContext& ctx,
              uint64_t reg_offset,
              uint64_t reg_size);

    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
};
