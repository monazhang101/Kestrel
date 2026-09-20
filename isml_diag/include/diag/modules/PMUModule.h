#pragma once

#include "diag/core/TestTarget.h"

class PMUModule : public TestTarget {
private:
    TestStatus example(TestInfo& ti);

public:
    PMUModule(const std::string& name, const DeviceContext& ctx,
                const ModuleInstanceConfig& config);

    uint64_t reg_base_offset() const { return ctx_.reg_base_offset; }
    uint64_t reg_size() const { return ctx_.reg_size; }
    uint32_t bar_index() const { return ctx_.reg_bar_index; }
};
