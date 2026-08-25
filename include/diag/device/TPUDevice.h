#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"
#include "diag/implementer/Implementer.h"
#include "diag/implementer/TPUImpl.h"
#include "diag/module/DDPModule.h"
#include "diag/module/ISIModule.h"
#include "diag/module/PCIeModule.h"
#include "diag/module/PMUModule.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TPUDevice : public BaseDevice {
private:
    uint32_t tpu_index_ = 0;
    TPUDeviceConfig config_;
    std::shared_ptr<Implementer> implementer_;
    std::unique_ptr<TPUImpl> impl_;

    std::unique_ptr<PCIeModule> pcie_;
    std::unique_ptr<PMUModule> pmu_;
    std::vector<std::unique_ptr<ISIModule>> isi_modules_;
    std::vector<std::unique_ptr<DDPModule>> ddp_modules_;

    TestResult Identify(TestInfo& ti);

public:
    TPUDevice(const std::string& logical_name,
              const DeviceContext& ctx,
              const TPUDeviceConfig& config,
              std::shared_ptr<Implementer> implementer);

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
