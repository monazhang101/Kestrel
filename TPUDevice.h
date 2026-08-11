#pragma once

#include "BaseDevice.h"
#include "DDPModule.h"
#include "ISIModule.h"
#include "PMUModule.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TPUDevice : public BaseDevice {
private:
    static constexpr uint64_t PCIE_REG_OFFSET = 0x0000;
    static constexpr uint64_t PCIE_REG_SIZE   = 0x1000;

    static constexpr uint64_t ISI0_REG_OFFSET = 0x1000;
    static constexpr uint64_t ISI1_REG_OFFSET = 0x2000;
    static constexpr uint64_t ISI_REG_SIZE    = 0x1000;

    static constexpr uint64_t PMU_REG_OFFSET  = 0x3000;
    static constexpr uint64_t PMU_REG_SIZE    = 0x1000;

    static constexpr uint64_t DDP_REG_OFFSET  = 0x5000;
    static constexpr uint64_t DDP_REG_SIZE    = 0x1000;

    static constexpr uint64_t SOC_REG_OFFSET  = 0x6000;
    static constexpr uint64_t SOC_REG_SIZE    = 0x1000;

    uint32_t tpu_index_ = 0;
    void* pcie_reg_base_ = nullptr;
    uint64_t pcie_reg_size_ = 0;
    void* soc_reg_base_ = nullptr;
    uint64_t soc_reg_size_ = 0;

    std::unique_ptr<PMUModule> pmu_;
    std::vector<std::unique_ptr<ISIModule>> isi_modules_;
    std::unique_ptr<DDPModule> ddp_;

    TestResult Identify(const TestArgs& args);
    TestResult BarProbe(const TestArgs& args);
    TestResult RegisterBlockProbe(const TestArgs& args);
    TestResult ErrorCounterSnapshot(const TestArgs& args);

    TestResult SocGpioDirSet(const TestArgs& args);
    TestResult SocGpioRead(const TestArgs& args);
    TestResult SocGpioWrite(const TestArgs& args);

    TestResult PcieEnumCheck(const TestArgs& args);
    TestResult PcieLinkStatusCheck(const TestArgs& args);
    TestResult PcieCapListCheck(const TestArgs& args);
    TestResult PcieExtCapListCheck(const TestArgs& args);
    TestResult PcieRegScan(const TestArgs& args);
    TestResult PciePmuRegScan(const TestArgs& args);
    TestResult PcieBarSizeGet(const TestArgs& args);
    TestResult PcieDmemMmioScan(const TestArgs& args);
    TestResult PcieDmemHdmaScan(const TestArgs& args);
    TestResult PcieDmemRegScan(const TestArgs& args);
    TestResult PcieVfioHdmaIntrSetup(const TestArgs& args);
    TestResult PcieVfioWaitMsiIntr(const TestArgs& args);
    TestResult PcieParallelDmaWithCompare(const TestArgs& args);
    TestResult PcieMcIntrIsSet(const TestArgs& args);
    TestResult PciePmuIntrTrigger(const TestArgs& args);
    TestResult PciePmuIntrIsSet(const TestArgs& args);
    TestResult PcieIsiIntrIsSet(const TestArgs& args);
    TestResult PcieLinkSpeedChange(const TestArgs& args);
    TestResult PcieDmaDataTransfer(const TestArgs& args);

public:
    TPUDevice(const std::string& logical_name,
              const DeviceContext& ctx,
              uint32_t tpu_index);

    TPUDevice(const TPUDevice&) = delete;
    TPUDevice& operator=(const TPUDevice&) = delete;

    ~TPUDevice() override = default;

    uint32_t tpu_index() const { return tpu_index_; }
    PMUModule* pmu() const { return pmu_.get(); }
    ISIModule* isi(size_t index) const;
    DDPModule* ddp() const { return ddp_.get(); }

    std::vector<BaseDevice*> child_targets() const;
    void print_tree() const;
};
