#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"
#include "diag/implementer/PMUImpl.h"

#include <cstdint>
#include <memory>
#include <string>

class PMUModule : public BaseDevice {
private:
    void* reg_base_ = nullptr;
    uint32_t bar_index_ = 0;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<PMUImpl> impl_;

    /* ----------- Register atomic tests here ----------- */
    TestResult PmuIpcRequestStart(TestInfo& ti);
    TestResult PmuIpcRequestExec(TestInfo& ti);
    TestResult PmuIpcRequestFinish(TestInfo& ti);
    TestResult PmuRegRead(TestInfo& ti);
    TestResult PmuRegWrite(TestInfo& ti);
    TestResult PmuRegCheck(TestInfo& ti);

public:
    PMUModule(const std::string& name,
              const DeviceContext& ctx,
              const ModuleInstanceConfig& config,
              std::unique_ptr<PMUImpl> impl);

    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
    uint32_t bar_index() const { return bar_index_; }
};
