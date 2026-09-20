#pragma once

#include "diag/core/TestTarget.h"

class DDPModule : public TestTarget {
private:
    uint32_t ddp_id_ = 0;
    TestStatus example(TestInfo& ti);

public:
    DDPModule(const std::string& name, const DeviceContext& ctx,
                const ModuleInstanceConfig& config);

    uint32_t ddp_id() const { return ddp_id_; }
    uint64_t reg_base_offset() const { return ctx_.reg_base_offset; }
    uint64_t reg_size() const { return ctx_.reg_size; }
    uint32_t bar_index() const { return ctx_.reg_bar_index; }
};
