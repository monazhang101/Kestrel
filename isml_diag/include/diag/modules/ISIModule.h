#pragma once

#include "diag/core/TestTarget.h"

class ISIModule : public TestTarget {
private:
    uint32_t link_id_ = 0;
    TestStatus example(TestInfo& ti);

public:
    ISIModule(const std::string& name, const DeviceContext& ctx,
                const ModuleInstanceConfig& config);

    uint32_t link_id() const { return link_id_; }
    uint64_t reg_base_offset() const { return ctx_.reg_base_offset; }
    uint64_t reg_size() const { return ctx_.reg_size; }
    uint32_t bar_index() const { return 0; }
};
