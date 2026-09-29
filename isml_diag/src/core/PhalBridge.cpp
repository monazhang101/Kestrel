#include "diag/core/PhalBridge.h"

#include "diag/core/Common.h"

#include <sstream>
#include <utility>

// PHAL lives in the standalone isml_ipc/ProtonHal repo. Keep real component
// headers at direct PHAL call sites, such as module testcases and DevMem.cpp.
//
// Expected PHAL include layout from isml_ipc/external/ProtonHal:
//   <phal/phal.h> provides phal_ctx_t, phal_config_t, phal_init/deinit,
//   env_t, hw_ops_t, and phal_status_t.
extern "C" {
#include <phal/phal.h>
}

phal_status_t ihal_write32(void* handler, uintptr_t addr, uint32_t data);
phal_status_t ihal_read32(void* handler, uintptr_t addr, uint32_t* data);

namespace {

void log_phal_io(const char* operation, env_t* env, uintptr_t addr,
                  uint32_t data, phal_status_t status)
{
    const auto* io = static_cast<const IhalIO*>(env->priv);
    auto* logger = io->device_ctx != nullptr ? io->device_ctx->logger : nullptr;
    if (logger == nullptr || (status == PHAL_STATUS_OK && logger->get_level() < LogLevel::Debug)) return;
    using common::format::hex;
    const auto message = std::string(operation) + " bdf=" + io->device_ctx->bdf +
        " env_base=" + hex(env->base) + " offset=" + hex(addr) +
        " bar0_offset=" + hex(env->base + addr) + " value=" + hex(data, 8) +
        " status=" + std::to_string(static_cast<int>(status));
    if (status == PHAL_STATUS_OK) logger->debug(message);
    else logger->error(message);
}

static phal_status_t hal_drv_write(env_t* env, uintptr_t addr, uint32_t data)
{
    if (env == nullptr || env->priv == nullptr) {
        return PHAL_STATUS_INVALID;
    }
    const auto status = ihal_write32(env->priv, env->base + addr, data);
    log_phal_io("phal_write", env, addr, data, status);
    return status;
}

static phal_status_t hal_drv_read(env_t* env, uintptr_t addr, uint32_t* data)
{
    if (env == nullptr || env->priv == nullptr || data == nullptr) {
        return PHAL_STATUS_INVALID;
    }
    const auto status = ihal_read32(env->priv, env->base + addr, data);
    log_phal_io("phal_read", env, addr, status == PHAL_STATUS_OK ? *data : 0, status);
    return status;
}

hw_ops_t make_hal_drv_ops()
{
    hw_ops_t ops{};
    ops.write = hal_drv_write;
    ops.read = hal_drv_read;
    return ops;
}

const hw_ops_t hal_drv_ops = make_hal_drv_ops();

const char* phal_project_macro(PhalProject project)
{
    switch (project) {
    case PhalProject::Atlas:
        return "PHAL_PROJECT_ATLAS";
    case PhalProject::AtlasM:
        return "PHAL_PROJECT_ATLAS_M";
    case PhalProject::Generic:
        return "PHAL_PROJECT_GENERIC";
    }
    return "PHAL_PROJECT_GENERIC";
}

auto to_phal_project(PhalProject project)
{
    switch (project) {
    case PhalProject::Atlas:
        return PHAL_PROJECT_ATLAS;
    case PhalProject::AtlasM:
        return PHAL_PROJECT_ATLAS_M;
    case PhalProject::Generic:
        return PHAL_PROJECT_GENERIC;
    }
    return PHAL_PROJECT_GENERIC;
}

std::string phal_error(const char* api, phal_status_t status)
{
    std::ostringstream stream;
    stream << api << " failed: status=" << static_cast<int>(status);
    return stream.str();
}

}

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

phal_status_t ihal_write32(void* handler, uintptr_t addr, uint32_t data)
{
    auto* ihalIO = static_cast<IhalIO*>(handler);
    if (ihalIO == nullptr || ihalIO->device_ctx == nullptr) {
        return PHAL_STATUS_INVALID;
    }

    return common::bar::write32(*ihalIO->device_ctx,
                                0,
                                static_cast<uint64_t>(addr),
                                data)
               ? PHAL_STATUS_OK
               : PHAL_STATUS_INVALID;
}

phal_status_t ihal_read32(void* handler, uintptr_t addr, uint32_t* data)
{
    auto* ihalIO = static_cast<IhalIO*>(handler);
    if (ihalIO == nullptr || ihalIO->device_ctx == nullptr || data == nullptr) {
        return PHAL_STATUS_INVALID;
    }

    return common::bar::read32(*ihalIO->device_ctx,
                               0,
                               static_cast<uint64_t>(addr),
                               *data)
               ? PHAL_STATUS_OK
               : PHAL_STATUS_INVALID;
}

class PhalBridge::ScopedContext::State {
public:
    Logger logger;
    DeviceContext device_ctx;
    IhalIO ihalIO;
    phal_ctx_t ctx{};
    bool initialized = false;

    ~State()
    {
        if (initialized) {
            phal_deinit(&ctx);
        }
    }
};

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

PhalBridge::ScopedContext PhalBridge::create_context(const DeviceContext& ctx,
                                                      PhalProject project,
                                                      std::string* error)
{
    if (project == PhalProject::Generic) {
        project = phal_project_from_tpu_type(ctx.tpu_type);
    }

    auto state = std::make_unique<ScopedContext::State>();
    state->device_ctx = ctx;
    state->logger.set_level(ctx.logger ? ctx.logger->get_level() : LogLevel::Info);
    state->device_ctx.logger = &state->logger;
    state->ihalIO.device_ctx = &state->device_ctx;

    phal_config_t config{};
    config.env_config.base = CHIP_RCF_BASE;
    config.env_config.user_data = &state->ihalIO;
#if !defined(HAL_FIRMWARE_MODE)
    config.env_config.hw_ops = &hal_drv_ops;
#endif
    config.project_config = to_phal_project(project);

    auto status = phal_init(&state->ctx, &config);
    if (status != PHAL_STATUS_OK) {
        if (error != nullptr) {
            *error = phal_error("phal_init", status);
            *error += " project=";
            *error += phal_project_macro(project);
            *error += " bdf=";
            *error += ctx.bdf;
            *error += " chip_rcf_base=0x20000000";
        }
        return {};
    }

    state->initialized = true;
    // Keep the framework-owned chip root explicit even if a PHAL project
    // performs additional initialization internally.
    state->ctx.env.base = CHIP_RCF_BASE;
    return ScopedContext(std::move(state));
}
