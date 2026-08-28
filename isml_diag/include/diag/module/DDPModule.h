#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"
#include "diag/implementer/DDPImpl.h"
#include "diag/implementer/Implementer.h"
#include "diag/module/DMCModule.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class DDPModule : public BaseDevice {
private:
    uint32_t ddp_id_ = 0;
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<DDPImpl> impl_;
    std::vector<std::unique_ptr<DMCModule>> dmc_modules_;

    /* ----------- Register atomic tests here ----------- */
    TestResult DdpDmemLinkupVerify(TestInfo& ti);

public:
    DDPModule(const std::string& name,
              const DeviceContext& ctx,
              const DDPModuleConfig& config,
              std::unique_ptr<DDPImpl> impl,
              const std::shared_ptr<Implementer>& implementer);

    uint32_t ddp_id() const { return ddp_id_; }
    DMCModule* dmc(size_t index) const;
    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
    std::vector<BaseDevice*> child_targets() const override;
};
