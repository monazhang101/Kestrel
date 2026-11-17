#pragma once

#include "diag/core/Platform.h"
#include "diag/core/TestInfo.h"
#include "diag/implementer/PCIeImpl.h"

#include <memory>

class Implementer {
private:
    TPUType tpu_type_ = TPUType::Unknown;

public:
    explicit Implementer(TPUType tpu_type);

    TPUType tpu_type() const { return tpu_type_; }

    MemoryRegionMap memory_regions() const;
    std::unique_ptr<PCIeImpl> pcie_impl() const;
};
