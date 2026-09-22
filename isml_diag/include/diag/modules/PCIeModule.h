#pragma once

#include "diag/core/TestTarget.h"
#include "diag/implementer/PCIeImpl.h"

#include <cstdint>
#include <memory>
#include <string>

class PCIeModule : public TestTarget {
private:
    uint32_t bar_index_ = 0;
    uint64_t reg_base_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<PCIeImpl> impl_;

    // Testcase implementations registered by PCIeModule's constructor.
    TestStatus bar_read32(TestInfo& ti);
    TestStatus bar_read32_abs(TestInfo& ti);
    TestStatus bar_scan32(TestInfo& ti);
    TestStatus example(TestInfo& ti);
    TestStatus sequential_aperture_mapping(TestInfo& ti);
    TestStatus dma_data_transfer(TestInfo& ti);

public:
    PCIeModule(const std::string& name,
               const DeviceContext& ctx,
               const ModuleInstanceConfig& config,
               std::unique_ptr<PCIeImpl> impl);

    uint64_t reg_base_offset() const { return reg_base_offset_; }
    uint64_t reg_size() const { return reg_size_; }
    uint32_t bar_index() const { return bar_index_; }
};
