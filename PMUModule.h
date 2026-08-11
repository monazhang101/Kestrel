#pragma once

#include "BaseDevice.h"

#include <cstdint>
#include <string>

class PMUModule : public BaseDevice {
private:
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;

    TestResult PmuIpcRequestStart(const TestArgs& args);
    TestResult PmuIpcRequestExec(const TestArgs& args);
    TestResult PmuIpcRequestFinish(const TestArgs& args);
    TestResult PmuRegRead(const TestArgs& args);
    TestResult PmuRegWrite(const TestArgs& args);
    TestResult PmuRegExpect(const TestArgs& args);
    TestResult PmuSoftResetTrigger(const TestArgs& args);
    TestResult PmuPcieCfgBackup(const TestArgs& args);
    TestResult PmuPcieCfgRestore(const TestArgs& args);
    TestResult PmuEccTrigger(const TestArgs& args);
    TestResult PmuEccClean(const TestArgs& args);
    TestResult PmuEccSnapshotAssert(const TestArgs& args);

public:
    PMUModule(const std::string& name,
              const DeviceContext& ctx,
              uint64_t reg_offset,
              uint64_t reg_size);

    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
};
