#include "diag/core/PhalBridge.h"

#include "diag/core/Common.h"

#include <sstream>
#include <utility>

// PHAL lives in the standalone isml_ipc/ProtonHal repo. Keep all real PHAL
// headers and types in this translation unit so the rest of diag only sees the
// small PhalBridge/DevMem contract.
//
// Expected PHAL include layout from isml_ipc/external/ProtonHal:
//   <phal/phal.h> provides phal_ctx_t, phal_config_t, phal_init/deinit,
//   env_t, hw_ops_t, and phal_status_t.
//   <phal/components/pcie/pcie.h> provides phal_pcie_aperture_set/get.
extern "C" {
#include <phal/phal.h>
#include <phal/components/pcie/pcie.h>
}

phal_status_t ihal_write32(void* handler, uintptr_t addr, uint32_t data);
phal_status_t ihal_read32(void* handler, uintptr_t addr, uint32_t* data);

namespace {

static phal_status_t hal_drv_write(env_t* env, uintptr_t addr, uint32_t data)
{
    if (env == nullptr || env->priv == nullptr) {
        return PHAL_STATUS_INVALID;
    }
    return ihal_write32(env->priv, addr, data);
}

static phal_status_t hal_drv_read(env_t* env, uintptr_t addr, uint32_t* data)
{
    if (env == nullptr || env->priv == nullptr || data == nullptr) {
        return PHAL_STATUS_INVALID;
    }
    return ihal_read32(env->priv, addr, data);
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

    // PHAL passes an offset inside its PCIe CSR block. The policy-provided
    // module base makes it a BAR0-relative offset for our iHAL BAR access.
    const auto offset = ihalIO->module_base + static_cast<uint64_t>(addr);
    return common::bar::write32(*ihalIO->device_ctx,
                                ihalIO->bar_index,
                                offset,
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

    const auto offset = ihalIO->module_base + static_cast<uint64_t>(addr);
    return common::bar::read32(*ihalIO->device_ctx,
                               ihalIO->bar_index,
                               offset,
                               *data)
               ? PHAL_STATUS_OK
               : PHAL_STATUS_INVALID;
}

class PhalBridge::Impl {
public:
    phal_ctx_t ctx{};
    IhalIO ihalIO{};
    bool initialized = false;

    ~Impl()
    {
        if (initialized) {
            phal_deinit(&ctx);
        }
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
    if (impl_ == nullptr) {
        impl_ = std::make_unique<Impl>();
        return;
    }
    if (impl_->initialized) {
        phal_deinit(&impl_->ctx);
    }
    impl_ = std::make_unique<Impl>();
}

bool PhalBridge::init(const IhalIO& ihalIO, PhalProject project, std::string* error)
{
    if (ihalIO.device_ctx == nullptr) {
        if (error != nullptr) {
            *error = "ihalIO.device_ctx is null";
        }
        return false;
    }

    if (impl_ == nullptr) {
        impl_ = std::make_unique<Impl>();
    }

    if (impl_->initialized) {
        phal_deinit(&impl_->ctx);
        impl_->ctx = {};
        impl_->initialized = false;
    }

    impl_->ihalIO = ihalIO;

    phal_config_t config{};
    config.env_config.base = 0;
    config.env_config.user_data = &impl_->ihalIO;
#if !defined(HAL_FIRMWARE_MODE)
    config.env_config.hw_ops = &hal_drv_ops;
#endif
    config.project_config = to_phal_project(project);

    auto status = phal_init(&impl_->ctx, &config);
    if (status != PHAL_STATUS_OK) {
        if (error != nullptr) {
            *error = phal_error("phal_init", status);
            *error += " project=";
            *error += phal_project_macro(project);
        }
        return false;
    }

    impl_->initialized = true;
    return true;
}

bool PhalBridge::initialized() const
{
    return impl_ != nullptr && impl_->initialized;
}

void* PhalBridge::native_context() const
{
    return initialized() ? &impl_->ctx : nullptr;
}

DevMem::DevMem(DeviceContext ctx, DevMemSpec spec, PhalBridge* phal, Logger* logger)
    : ctx_(std::move(ctx)),
      spec_(spec),
      phal_(phal),
      logger_(logger),
      valid_(spec.size != 0 && phal != nullptr)
{
}

DevMem DevMem::invalid(std::string error)
{
    DevMem dev_mem;
    dev_mem.error_ = std::move(error);
    return dev_mem;
}

bool DevMem::range_ok(uint64_t offset, size_t len) const
{
    if (!valid_ || len == 0 || offset > spec_.size) {
        return false;
    }
    return static_cast<uint64_t>(len) <= (spec_.size - offset);
}

void DevMem::log_info(const std::string& message) const
{
    if (logger_ != nullptr) {
        logger_->info(message);
    }
}

void DevMem::log_error(const std::string& message) const
{
    if (logger_ != nullptr) {
        logger_->error(message);
    }
}

bool DevMem::read(uint64_t offset, void* data, size_t len) const
{
    if (data == nullptr || !range_ok(offset, len)) {
        return false;
    }

    auto* phal_ctx = static_cast<phal_ctx_t*>(phal_->native_context());
    if (phal_ctx == nullptr) {
        log_error("dev_mem read aperture_set failed: phal context is not initialized");
        return false;
    }

    phal_pcie_aperture_t apt = {};
    apt.identity = spec_.identity;
    apt.target_addr = spec_.target_addr;
    apt.size = spec_.size;

    auto status = phal_pcie_aperture_set(phal_ctx,
                                         spec_.dmem_bar_index,
                                         spec_.aperture_index,
                                         &apt);
    if (status != PHAL_STATUS_OK) {
        log_error("dev_mem read aperture_set failed: " +
                  phal_error("phal_pcie_aperture_set", status));
        return false;
    }

    apt = {};
    status = phal_pcie_aperture_get(phal_ctx,
                                    spec_.dmem_bar_index,
                                    spec_.aperture_index,
                                    &apt);
    if (status != PHAL_STATUS_OK) {
        log_error("dev_mem read aperture_get failed: " +
                  phal_error("phal_pcie_aperture_get", status));
        return false;
    }

    std::ostringstream stream;
    stream << "dev_mem read aperture configured: bar="
           << static_cast<uint32_t>(spec_.dmem_bar_index)
           << " aperture=" << static_cast<uint32_t>(spec_.aperture_index)
           << " identity=" << static_cast<uint32_t>(apt.identity)
           << " target_addr=0x" << std::hex << apt.target_addr
           << " size=0x" << apt.size
           << " bar_offset=0x" << spec_.aperture_bar_offset;
    log_info(stream.str());

    return common::bar::read(ctx_,
                             spec_.dmem_bar_index,
                             spec_.aperture_bar_offset + offset,
                             data,
                             len);
}

bool DevMem::write(uint64_t offset, const void* data, size_t len) const
{
    if (data == nullptr || spec_.readonly || !range_ok(offset, len)) {
        return false;
    }

    auto* phal_ctx = static_cast<phal_ctx_t*>(phal_->native_context());
    if (phal_ctx == nullptr) {
        log_error("dev_mem write aperture_set failed: phal context is not initialized");
        return false;
    }

    phal_pcie_aperture_t apt = {};
    apt.identity = spec_.identity;
    apt.target_addr = spec_.target_addr;
    apt.size = spec_.size;

    auto status = phal_pcie_aperture_set(phal_ctx,
                                         spec_.dmem_bar_index,
                                         spec_.aperture_index,
                                         &apt);
    if (status != PHAL_STATUS_OK) {
        log_error("dev_mem write aperture_set failed: " +
                  phal_error("phal_pcie_aperture_set", status));
        return false;
    }

    apt = {};
    status = phal_pcie_aperture_get(phal_ctx,
                                    spec_.dmem_bar_index,
                                    spec_.aperture_index,
                                    &apt);
    if (status != PHAL_STATUS_OK) {
        log_error("dev_mem write aperture_get failed: " +
                  phal_error("phal_pcie_aperture_get", status));
        return false;
    }

    std::ostringstream stream;
    stream << "dev_mem write aperture configured: bar="
           << static_cast<uint32_t>(spec_.dmem_bar_index)
           << " aperture=" << static_cast<uint32_t>(spec_.aperture_index)
           << " identity=" << static_cast<uint32_t>(apt.identity)
           << " target_addr=0x" << std::hex << apt.target_addr
           << " size=0x" << apt.size
           << " bar_offset=0x" << spec_.aperture_bar_offset;
    log_info(stream.str());

    return common::bar::write(ctx_,
                              spec_.dmem_bar_index,
                              spec_.aperture_bar_offset + offset,
                              data,
                              len);
}
