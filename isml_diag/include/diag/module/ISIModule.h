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
    uint32_t bar_index_ = 0;
    uint64_t reg_base_offset_ = 0;
    uint64_t reg_size_ = 0;
    std::unique_ptr<ISIImpl> impl_;

    /* ----------- Register atomic tests here ----------- */
    TestResult IsiLinkup(TestInfo& ti);
    TestResult IsiSetup(TestInfo& ti);
    TestResult IsiPcieApertureContext(TestInfo& ti);
    TestResult IsiCommonDevMemRead(TestInfo& ti);

public:
    ISIModule(const std::string& name,
              const DeviceContext& ctx,
              const ModuleInstanceConfig& config,
              std::unique_ptr<ISIImpl> impl);

    uint32_t link_id() const { return link_id_; }
    uint64_t reg_base_offset() const { return reg_base_offset_; }
    uint64_t reg_size() const { return reg_size_; }
    uint32_t bar_index() const { return bar_index_; }
};
