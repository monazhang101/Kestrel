#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"
#include "diag/implementer/DDPImpl.h"
#include "diag/implementer/DMCImpl.h"
#include "diag/implementer/ISIImpl.h"
#include "diag/implementer/PCIeImpl.h"
#include "diag/implementer/PMUImpl.h"
#include "diag/implementer/TPUImpl.h"

#include <cstdint>
#include <memory>
#include <string>

struct ModuleImplContext {
    std::string target_name;
    DeviceContext device_ctx;
    uint32_t index = 0;
    uint32_t parent_index = 0;
    uint32_t bar_index = 0;
    void* reg_base = nullptr;
    uint64_t reg_offset = 0;
    uint64_t reg_size = 0;
};

class Implementer {
private:
    TPUType tpu_type_ = TPUType::Unknown;

public:
    explicit Implementer(TPUType tpu_type);

    TPUType tpu_type() const { return tpu_type_; }

    std::unique_ptr<TPUImpl> tpu_impl(const ModuleImplContext& ctx) const;
    std::unique_ptr<PCIeImpl> pcie_impl(const ModuleImplContext& ctx) const;
    std::unique_ptr<PMUImpl> pmu_impl(const ModuleImplContext& ctx) const;
    std::unique_ptr<ISIImpl> isi_impl(const ModuleImplContext& ctx) const;
    std::unique_ptr<DDPImpl> ddp_impl(const ModuleImplContext& ctx) const;
    std::unique_ptr<DMCImpl> dmc_impl(const ModuleImplContext& ctx) const;
};
