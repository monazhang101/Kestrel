#pragma once

#include "diag/core/TestTarget.h"
#include "diag/implementer/PMUImpl.h"

#include <cstdint>
#include <memory>
#include <string>

class PMUModule : public TestTarget {
private:
    uint32_t bar_index_ = 0;
    uint64_t reg_base_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<PMUImpl> impl_;

    // Testcase implementations registered by PMUModule's constructor.
    TestStatus ipc_request_start(TestInfo& ti);
    TestStatus ipc_request_exec(TestInfo& ti);
    TestStatus ipc_request_finish(TestInfo& ti);
    TestStatus reg_read(TestInfo& ti);
    TestStatus reg_write(TestInfo& ti);
    TestStatus reg_check(TestInfo& ti);

public:
    PMUModule(const std::string& name,
              const DeviceContext& ctx,
              const ModuleInstanceConfig& config,
              std::unique_ptr<PMUImpl> impl);

    uint64_t reg_base_offset() const { return reg_base_offset_; }
    uint64_t reg_size() const { return reg_size_; }
    uint32_t bar_index() const { return bar_index_; }
};
