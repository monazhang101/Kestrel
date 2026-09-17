#pragma once

#include "diag/core/TestTarget.h"
#include "diag/implementer/Implementer.h"
#include "diag/implementer/TPUImpl.h"
#include "diag/modules/DDPModule.h"
#include "diag/modules/ISIModule.h"
#include "diag/modules/PCIeModule.h"
#include "diag/modules/PMUModule.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TPUDevice : public TestTarget {
private:
    uint32_t tpu_index_ = 0;
    TPUDeviceConfig config_;
    std::shared_ptr<Implementer> implementer_;
    std::unique_ptr<TPUImpl> impl_;

    std::unique_ptr<PCIeModule> pcie_;
    std::unique_ptr<PMUModule> pmu_;
    std::vector<std::unique_ptr<ISIModule>> isi_modules_;
    std::vector<std::unique_ptr<DDPModule>> ddp_modules_;

    TestStatus identify(TestInfo& ti);

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

    std::vector<TestTarget*> child_targets() const override;
    void print_tree() const;
};
