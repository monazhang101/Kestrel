#pragma once

#include "BaseDevice.h"

#include <cstdint>
#include <string>

class ISIModule : public BaseDevice {
private:
    uint32_t link_id_ = 0;
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;

    TestResult IsiLinkup(const TestArgs& args);
    TestResult IsiSetup(const TestArgs& args);
    TestResult IsiIsrTblWriteEnable(const TestArgs& args);
    TestResult IsiIsrTblWriteDisable(const TestArgs& args);
    TestResult IsiIsrTblSet(const TestArgs& args);
    TestResult IsiIsrTblDirectSet(const TestArgs& args);
    TestResult IsiIsrTblGet(const TestArgs& args);
    TestResult IsiPcsLoopback(const TestArgs& args);
    TestResult IsiPhyLoopback(const TestArgs& args);
    TestResult IsiPhyBistTest(const TestArgs& args);
    TestResult IsiIfcIntrEnable(const TestArgs& args);
    TestResult IsiIfcIntrIsSet(const TestArgs& args);
    TestResult IsiIfcNodeIdSet(const TestArgs& args);
    TestResult IsiXdcAddrSet(const TestArgs& args);
    TestResult IsiSendPacket(const TestArgs& args);
    TestResult IsiRegScan(const TestArgs& args);
    TestResult IsiPollCreditRegs(const TestArgs& args);
    TestResult IsiDumpDebugRegs(const TestArgs& args);
    TestResult IsiPciePmuIntrIsSet(const TestArgs& args);

public:
    ISIModule(const std::string& name,
              const DeviceContext& ctx,
              uint32_t link_id,
              uint64_t reg_offset,
              uint64_t reg_size);

    uint32_t link_id() const { return link_id_; }
    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
};
