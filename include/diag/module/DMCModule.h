#pragma once

#include "diag/core/BaseDevice.h"

#include <cstdint>
#include <string>

class DMCModule : public BaseDevice {
private:
    uint32_t ddp_id_ = 0;
    uint32_t controller_id_ = 0;
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;

    TestResult DmcStatusCheck(TestInfo& ti);
    TestResult DmcRegScan(TestInfo& ti);

public:
    DMCModule(const std::string& name,
              const DeviceContext& ctx,
              uint32_t ddp_id,
              uint32_t controller_id,
              uint64_t reg_offset,
              uint64_t reg_size);

    uint32_t ddp_id() const { return ddp_id_; }
    uint32_t controller_id() const { return controller_id_; }
    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
};
