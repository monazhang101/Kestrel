#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"
#include "diag/implementer/ISIImpl.h"

#include <cstdint>
#include <memory>
#include <string>

class ISIModule : public BaseDevice {
private:
    uint32_t link_id_ = 0;
    void* reg_base_ = nullptr;
    uint64_t reg_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<ISIImpl> impl_;

    /* ----------- Register atomic tests here ----------- */
    TestResult IsiLinkup(TestInfo& ti);
    TestResult IsiSetup(TestInfo& ti);

public:
    ISIModule(const std::string& name,
              const DeviceContext& ctx,
              const ModuleInstanceConfig& config,
              std::unique_ptr<ISIImpl> impl);

    uint32_t link_id() const { return link_id_; }
    uint64_t reg_offset() const { return reg_offset_; }
    uint64_t reg_size() const { return reg_size_; }
};
