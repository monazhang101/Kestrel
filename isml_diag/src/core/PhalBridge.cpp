#include "diag/core/PhalBridge.h"

#include "diag/core/Common.h"

#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

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

static phal_status_t hal_drv_write(env_t* env, uintptr_t addr, uint32_t data)
{
    if (env == nullptr || env->priv == nullptr) {
        return PHAL_STATUS_INVALID;
    }
    return ihal_write32(env->priv, env->base + addr, data);
}

static phal_status_t hal_drv_read(env_t* env, uintptr_t addr, uint32_t* data)
{
    if (env == nullptr || env->priv == nullptr || data == nullptr) {
        return PHAL_STATUS_INVALID;
    }
    return ihal_read32(env->priv, env->base + addr, data);
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
                                ihalIO->bar_index,
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
                               ihalIO->bar_index,
                               static_cast<uint64_t>(addr),
                               *data)
               ? PHAL_STATUS_OK
               : PHAL_STATUS_INVALID;
}

class PhalBridge::Impl {
public:
    struct ContextSlot {
        DeviceContext device_ctx;
        IhalIO ihalIO;
        uint32_t control_bar_index = 0;
        uint64_t base_offset = 0;
        PhalProject project = PhalProject::Generic;
        phal_ctx_t ctx{};
        bool initialized = false;

        ~ContextSlot()
        {
            if (initialized) {
                phal_deinit(&ctx);
            }
        }
    };

    std::mutex mutex;
    std::vector<std::unique_ptr<ContextSlot>> contexts;

    ContextSlot* find(const DeviceContext& device_ctx,
                      uint32_t control_bar_index,
                      uint64_t base_offset,
                      PhalProject project)
    {
        for (const auto& slot : contexts) {
            if (slot->device_ctx.bdf == device_ctx.bdf &&
                slot->control_bar_index == control_bar_index &&
                slot->base_offset == base_offset &&
                slot->project == project) {
                return slot.get();
            }
        }
        return nullptr;
    }
};

PhalBridge::PhalBridge()
    : impl_(std::make_unique<Impl>())
{
}

PhalBridge::~PhalBridge() = default;

PhalBridge::PhalBridge(PhalBridge&&) noexcept = default;

PhalBridge& PhalBridge::operator=(PhalBridge&&) noexcept = default;

void PhalBridge::reset()
{
    impl_ = std::make_unique<Impl>();
}

void* PhalBridge::get_context(const DeviceContext& ctx,
                              uint32_t control_bar_index,
                              uint64_t base_offset,
                              PhalProject project,
                              std::string* error)
{
    if (impl_ == nullptr) {
        impl_ = std::make_unique<Impl>();
    }

    if (project == PhalProject::Generic) {
        project = phal_project_from_tpu_type(ctx.tpu_type);
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (auto* existing = impl_->find(ctx, control_bar_index, base_offset, project)) {
        return &existing->ctx;
    }

    auto slot = std::make_unique<Impl::ContextSlot>();
    slot->device_ctx = ctx;
    slot->control_bar_index = control_bar_index;
    slot->base_offset = base_offset;
    slot->project = project;
    slot->ihalIO.device_ctx = &slot->device_ctx;
    slot->ihalIO.bar_index = control_bar_index;

    phal_config_t config{};
    config.env_config.base = base_offset;
    config.env_config.user_data = &slot->ihalIO;
#if !defined(HAL_FIRMWARE_MODE)
    config.env_config.hw_ops = &hal_drv_ops;
#endif
    config.project_config = to_phal_project(project);

    auto status = phal_init(&slot->ctx, &config);
    if (status != PHAL_STATUS_OK) {
        if (error != nullptr) {
            *error = phal_error("phal_init", status);
            *error += " project=";
            *error += phal_project_macro(project);
            *error += " bdf=";
            *error += ctx.bdf;
            *error += " control_bar=";
            *error += std::to_string(control_bar_index);
        }
        return nullptr;
    }

    slot->initialized = true;
    auto* phal_ctx = &slot->ctx;
    impl_->contexts.push_back(std::move(slot));
    return phal_ctx;
}
