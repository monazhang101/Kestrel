#pragma once

#include "diag/core/Platform.h"

extern "C" {
#include <phal/phal.h>
}

#include <atomic>
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
    std::atomic_uint32_t regio_suppression_depth{0};
};

// Suppress successful per-register PHAL logs within a bounded operation such
// as polling. Errors remain visible, and the caller emits sampled summaries.
class ScopedPhalRegIoSuppression {
private:
    IhalIO* io_ = nullptr;

public:
    explicit ScopedPhalRegIoSuppression(phal_ctx_t* ctx);
    ~ScopedPhalRegIoSuppression();

    ScopedPhalRegIoSuppression(const ScopedPhalRegIoSuppression&) = delete;
    ScopedPhalRegIoSuppression& operator=(const ScopedPhalRegIoSuppression&) = delete;
};

class PhalBridge {
public:
    // The Atlas RCF address space is rooted at this chip-level address.  It
    // is deliberately independent of any module register window.
    static constexpr uintptr_t CHIP_RCF_BASE = 0x20000000u;

    class ScopedContext {
    private:
        class State;
        std::unique_ptr<State> state_;
        explicit ScopedContext(std::unique_ptr<State> state);
        friend class PhalBridge;

    public:
        ScopedContext();
        ~ScopedContext();

        ScopedContext(const ScopedContext&) = delete;
        ScopedContext& operator=(const ScopedContext&) = delete;
        ScopedContext(ScopedContext&&) noexcept;
        ScopedContext& operator=(ScopedContext&&) noexcept;

        phal_ctx_t* get() const;
        explicit operator bool() const { return get() != nullptr; }
    };

    PhalBridge() = default;
    ~PhalBridge() = default;
    PhalBridge(const PhalBridge&) = delete;
    PhalBridge& operator=(const PhalBridge&) = delete;
    PhalBridge(PhalBridge&&) noexcept = default;
    PhalBridge& operator=(PhalBridge&&) noexcept = default;

    // Create one execution context rooted at the chip RCF address.  The
    // context is owned by the caller and is initialized/deinitialized by the
    // framework; no module base is part of this API.
    ScopedContext create_context(const DeviceContext& ctx,
                                 PhalProject project,
                                 std::string* error = nullptr);

};
