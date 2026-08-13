#pragma once

#include "BaseDevice.h"
#include "DMCModule.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class DDPModule : public BaseDevice {
private:
    static constexpr size_t DMC_COUNT = 3;
    static constexpr uint64_t DMC_REG_OFFSET = 0x400;
    static constexpr uint64_t DMC_REG_SIZE = 0x100;

    uint32_t ddp_id_ = 0;
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::vector<std::unique_ptr<DMCModule>> dmc_modules_;

    TestResult DdpBdfGetSecondaryBus(TestInfo& ti);
    TestResult DdpDvsecVerify(TestInfo& ti);
    TestResult DdpDvsecWalkChain(TestInfo& ti);
    TestResult DdpWidthVerify(TestInfo& ti);
    TestResult DdpDmemLinkupVerify(TestInfo& ti);
    TestResult DdpDmemPerf(TestInfo& ti);
    TestResult DdpMcLinkupIntrVerify(TestInfo& ti);
    TestResult DdpPcieRegScan(TestInfo& ti);
    TestResult DdpBistFifoRun(TestInfo& ti);

public:
    DDPModule(const std::string& name,
              const DeviceContext& ctx,
              uint32_t ddp_id,
              uint64_t reg_offset,
              uint64_t reg_size,
              std::shared_ptr<CmdQueueMgm> hqc_queue_mgm);

    uint32_t ddp_id() const { return ddp_id_; }
    DMCModule* dmc(size_t index) const;
    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
    std::vector<BaseDevice*> child_targets() const override;
};
