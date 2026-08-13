#pragma once

#include "BaseDevice.h"
#include "DDPModule.h"
#include "ISIModule.h"
#include "PCIeModule.h"
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

    static constexpr uint64_t PMU_REG_OFFSET  = 0x1000;
    static constexpr uint64_t PMU_REG_SIZE    = 0x1000;

    static constexpr uint64_t DDP_REG_OFFSET  = 0x2000;
    static constexpr uint64_t DDP_REG_SIZE    = 0x1000;
    static constexpr size_t DDP_COUNT         = 4;

    static constexpr uint64_t ISI_REG_OFFSET  = 0x6000;
    static constexpr uint64_t ISI_REG_SIZE    = 0x1000;
    static constexpr size_t ISI_COUNT         = 8;

    uint32_t tpu_index_ = 0;

    std::unique_ptr<PCIeModule> pcie_;
    std::unique_ptr<PMUModule> pmu_;
    std::vector<std::unique_ptr<ISIModule>> isi_modules_;
    std::vector<std::unique_ptr<DDPModule>> ddp_modules_;

    TestResult Identify(TestInfo& ti);

    TestResult SocGpioDirSet(TestInfo& ti);
    TestResult SocGpioRead(TestInfo& ti);
    TestResult SocGpioWrite(TestInfo& ti);

public:
    TPUDevice(const std::string& logical_name,
              const DeviceContext& ctx,
              uint32_t tpu_index);

    TPUDevice(const TPUDevice&) = delete;
    TPUDevice& operator=(const TPUDevice&) = delete;

    ~TPUDevice() override = default;

    uint32_t tpu_index() const { return tpu_index_; }
    PCIeModule* pcie() const { return pcie_.get(); }
    PMUModule* pmu() const { return pmu_.get(); }
    ISIModule* isi(size_t index) const;
    DDPModule* ddp(size_t index) const;

    std::vector<BaseDevice*> child_targets() const override;
    void print_tree() const;
};
