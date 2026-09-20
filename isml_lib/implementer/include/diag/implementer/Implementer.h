#pragma once

#include "diag/core/Platform.h"
#include "diag/core/TestInfo.h"
#include "diag/implementer/PCIeImpl.h"

#include <cstdint>
#include <memory>
#include <string>

struct ModuleImplContext {
    std::string target_name;
    DeviceContext device_ctx;
    uint32_t index = 0;
    uint32_t parent_index = 0;
    uint32_t bar_index = 0;
    uint64_t reg_base_offset = 0;
    uint64_t reg_size = 0;
};

class Implementer {
private:
    TPUType tpu_type_ = TPUType::Unknown;

public:
    explicit Implementer(TPUType tpu_type);

    TPUType tpu_type() const { return tpu_type_; }

    std::unique_ptr<PCIeImpl> pcie_impl(const ModuleImplContext& ctx) const;
};
