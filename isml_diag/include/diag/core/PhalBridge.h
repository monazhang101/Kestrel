#pragma once

#include "diag/core/Platform.h"
#include "diag/core/TestInfo.h"

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
    // All PHAL register callbacks use BAR0; base_offset is BAR0-relative.
    phal_ctx_t* get_context(const DeviceContext& ctx,
                      uint64_t base_offset,
                      PhalProject project,
                      std::string* error = nullptr);
};
