#pragma once

#include "diag/core/TestTarget.h"
#include "diag/implementer/DMCImpl.h"

#include <cstdint>
#include <memory>
#include <string>

class DMCModule : public TestTarget {
private:
    uint32_t ddp_id_ = 0;
    uint32_t controller_id_ = 0;
    uint32_t bar_index_ = 0;
    uint64_t reg_base_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<DMCImpl> impl_;

    // Testcase implementations registered by DMCModule's constructor.
    TestStatus status_check(TestInfo& ti);
    TestStatus reg_scan(TestInfo& ti);

public:
    DMCModule(const std::string& name,
              const DeviceContext& ctx,
              uint32_t ddp_id,
              const ModuleInstanceConfig& config,
              std::unique_ptr<DMCImpl> impl);

    uint32_t ddp_id() const { return ddp_id_; }
    uint32_t controller_id() const { return controller_id_; }
    uint64_t reg_base_offset() const { return reg_base_offset_; }
    uint64_t reg_size() const { return reg_size_; }
    uint32_t bar_index() const { return bar_index_; }
};
