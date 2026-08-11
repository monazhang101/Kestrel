#pragma once

#include "BaseDevice.h"

#include <cstdint>
#include <string>

class DDPModule : public BaseDevice {
private:
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;

    TestResult DdpBdfGetSecondaryBus(const TestArgs& args);
    TestResult DdpDvsecVerify(const TestArgs& args);
    TestResult DdpDvsecWalkChain(const TestArgs& args);
    TestResult DdpWidthVerify(const TestArgs& args);
    TestResult DdpDmemLinkupVerify(const TestArgs& args);
    TestResult DdpDmemPerf(const TestArgs& args);
    TestResult DdpMcLinkupIntrVerify(const TestArgs& args);
    TestResult DdpPcieRegScan(const TestArgs& args);
    TestResult DdpBistFifoRun(const TestArgs& args);

public:
    DDPModule(const std::string& name,
              const DeviceContext& ctx,
              uint64_t reg_offset,
              uint64_t reg_size);

    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
};
