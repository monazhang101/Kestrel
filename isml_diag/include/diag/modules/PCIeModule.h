#pragma once

#include "diag/core/TestTarget.h"
#include "diag/implementer/PCIeImpl.h"

#include <cstdint>
#include <memory>
#include <string>

class PCIeModule : public TestTarget {
private:
    std::unique_ptr<PCIeImpl> impl_;

    // Testcase implementations registered by PCIeModule's constructor.
    TestStatus bar_read32(TestInfo& ti);
    TestStatus example(TestInfo& ti);
    TestStatus sequential_aperture_mapping(TestInfo& ti);
    TestStatus dma_data_transfer(TestInfo& ti);

public:
    PCIeModule(const std::string& name,
               const DeviceContext& ctx,
               const ModuleInstanceConfig& config,
               std::unique_ptr<PCIeImpl> impl);

};
