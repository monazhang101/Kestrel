#pragma once

#include "diag/core/TestTarget.h"
#include "diag/implementer/AtlasResource.h"
#include "diag/implementer/Implementer.h"
#include "diag/modules/DDPModule.h"
#include "diag/modules/ISIModule.h"
#include "diag/modules/PCIeModule.h"
#include "diag/modules/PMUModule.h"
#include "diag/modules/SocModule.h"

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
    AtlasResourceInfo resource_info_;

    std::unique_ptr<PCIeModule> pcie_;
    std::unique_ptr<PMUModule> pmu_;
    std::unique_ptr<SocModule> soc_;
    std::vector<std::unique_ptr<ISIModule>> isi_modules_;
    std::vector<std::unique_ptr<DDPModule>> ddp_modules_;


public:
    TPUDevice(const std::string& logical_name,
              const DeviceContext& ctx,
              const TPUDeviceConfig& config,
              std::shared_ptr<Implementer> implementer,
              AtlasResourceInfo resource_info = {});

    TPUDevice(const TPUDevice&) = delete;
    TPUDevice& operator=(const TPUDevice&) = delete;

    ~TPUDevice() override = default;

    uint32_t tpu_index() const { return tpu_index_; }
    const AtlasResourceInfo& resource_info() const { return resource_info_; }
    bool has_isi(uint32_t index) const { return resource_info_.has_isi(index); }
    std::vector<uint32_t> isi_indices() const { return resource_info_.isi_indices(); }
    uint32_t dmc_mask() const { return resource_info_.dmc_mask; }
    bool has_dmc(uint32_t ddp, uint32_t dmc) const
    {
        return resource_info_.has_dmc(ddp, dmc);
    }
    uint32_t cxl_mask() const { return resource_info_.cxl_mask; }
    bool has_cxl(uint32_t ddp, uint32_t dmc) const
    {
        return resource_info_.has_cxl(ddp, dmc);
    }
    AtlasDmcType dmc_type(uint32_t ddp, uint32_t dmc) const
    {
        return resource_info_.get_dmc_type(ddp, dmc);
    }
    PCIeModule* pcie() const { return pcie_.get(); }
    PMUModule* pmu() const { return pmu_.get(); }
    ISIModule* isi(size_t index) const;
    DDPModule* ddp(size_t index) const;
    TestTarget* module(const std::string& type, uint32_t index) const;
    std::vector<std::string> get_registered_test_names() const override;

    std::vector<TestTarget*> child_targets() const override;
    void print_tree() const;
};
