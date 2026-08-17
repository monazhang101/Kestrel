#pragma once

#include "diag/core/BaseDevice.h"

#include <cstdint>
#include <string>

class ISIModule : public BaseDevice {
private:
    uint32_t link_id_ = 0;
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;

    TestResult IsiLinkup(TestInfo& ti);
    TestResult IsiSetup(TestInfo& ti);
    TestResult IsiIsrTblWriteEnable(TestInfo& ti);
    TestResult IsiIsrTblWriteDisable(TestInfo& ti);
    TestResult IsiIsrTblSet(TestInfo& ti);
    TestResult IsiIsrTblDirectSet(TestInfo& ti);
    TestResult IsiIsrTblGet(TestInfo& ti);
    TestResult IsiPcsLoopback(TestInfo& ti);
    TestResult IsiPhyLoopback(TestInfo& ti);
    TestResult IsiPhyBistTest(TestInfo& ti);
    TestResult IsiIfcIntrEnable(TestInfo& ti);
    TestResult IsiIfcIntrIsSet(TestInfo& ti);
    TestResult IsiIfcNodeIdSet(TestInfo& ti);
    TestResult IsiXdcAddrSet(TestInfo& ti);
    TestResult IsiSendPacket(TestInfo& ti);
    TestResult IsiRegScan(TestInfo& ti);
    TestResult IsiPollCreditRegs(TestInfo& ti);
    TestResult IsiDumpDebugRegs(TestInfo& ti);
    TestResult IsiPciePmuIntrIsSet(TestInfo& ti);

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
