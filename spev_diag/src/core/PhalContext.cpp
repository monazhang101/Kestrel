#include "PhalContextState.h"
#include <utility>

PhalProject phal_project_from_tpu_type(TPUType tpu_type)
{
    switch (tpu_type) {
    case TPUType::Atlas:
        return PhalProject::Atlas;
    case TPUType::AtlasM:
        return PhalProject::AtlasM;
    case TPUType::Unknown:
        return PhalProject::Generic;
    }
    return PhalProject::Generic;
}

ScopedPhalRegIoSuppression::ScopedPhalRegIoSuppression(phal_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->env.priv == nullptr) return;
    io_ = static_cast<IhalIO*>(ctx->env.priv);
    io_->regio_suppression_depth.fetch_add(1, std::memory_order_relaxed);
}

ScopedPhalRegIoSuppression::~ScopedPhalRegIoSuppression()
{
    if (io_ != nullptr) {
        io_->regio_suppression_depth.fetch_sub(1, std::memory_order_relaxed);
    }
}

PhalBridge::ScopedContext::ScopedContext(std::unique_ptr<State> state)
    : state_(std::move(state))
{
}

PhalBridge::ScopedContext::ScopedContext() = default;

PhalBridge::ScopedContext::~ScopedContext() = default;

PhalBridge::ScopedContext::ScopedContext(ScopedContext&&) noexcept = default;

PhalBridge::ScopedContext& PhalBridge::ScopedContext::operator=(ScopedContext&&) noexcept = default;

phal_ctx_t* PhalBridge::ScopedContext::get() const
{
    return state_ == nullptr ? nullptr : &state_->ctx;
}
