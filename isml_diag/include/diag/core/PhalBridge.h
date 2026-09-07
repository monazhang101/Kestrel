#pragma once

#include "diag/core/BaseDevice.h"
#include "diag/core/PlatformPolicy.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class PhalProject {
    Generic,
    Atlas,
    AtlasM,
};

PhalProject phal_project_from_tpu_type(TPUType tpu_type);

struct IhalIO {
    const DeviceContext* device_ctx = nullptr;
    uint32_t bar_index = 0;
};

class PhalBridge {
private:
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    PhalBridge();
    ~PhalBridge();

    PhalBridge(const PhalBridge&) = delete;
    PhalBridge& operator=(const PhalBridge&) = delete;
    PhalBridge(PhalBridge&&) noexcept;
    PhalBridge& operator=(PhalBridge&&) noexcept;

    void reset();
    void* get_context(const DeviceContext& ctx,
                      uint32_t control_bar_index,
                      uint64_t base_offset,
                      PhalProject project,
                      std::string* error = nullptr);
};
