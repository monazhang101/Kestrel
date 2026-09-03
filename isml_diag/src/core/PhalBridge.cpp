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

std::string hex_u64(uint64_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

std::string phal_aperture_call_log(const char* api,
                                   const phal_ctx_t* phal_ctx,
                                   uint32_t bar_index,
                                   uint8_t aperture_index,
                                   const phal_pcie_aperture_t& apt,
                                   phal_status_t status)
{
    std::ostringstream stream;
    stream << api
           << " ctx=" << hex_u64(reinterpret_cast<uintptr_t>(phal_ctx))
           << " bar_index=" << bar_index
           << " aperture_index=" << static_cast<uint32_t>(aperture_index)
           << " identity=" << static_cast<uint32_t>(apt.identity)
           << " target_addr=" << hex_u64(apt.target_addr)
           << " size=" << hex_u64(apt.size)
           << " status=" << static_cast<int>(status);
    return stream.str();
}

std::string devmem_log_context(const DevMemSpec& spec,
                               uint64_t offset,
                               size_t len,
                               const char* status)
{
    std::ostringstream stream;
    stream << "dmem_bar=" << static_cast<uint32_t>(spec.dmem_bar_index)
           << " aperture=" << static_cast<uint32_t>(spec.aperture_index)
           << " identity=" << static_cast<uint32_t>(spec.identity)
           << " target_addr=" << hex_u64(spec.target_addr)
           << " size=" << hex_u64(spec.size)
           << " aperture_bar_offset=" << hex_u64(spec.aperture_bar_offset)
           << " offset=" << hex_u64(offset)
           << " absolute_bar_offset=" << hex_u64(spec.aperture_bar_offset + offset)
           << " len=" << len
           << " pattern=" << (spec.pattern.empty() ? "n/a" : spec.pattern)
           << " status=" << status;
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

void DevMem::log_debug(const std::string& message) const
{
    if (logger_ != nullptr) {
        logger_->debug(message);
    }
}

void DevMem::log_error(const std::string& message) const
{
    if (logger_ != nullptr) {
        logger_->error(message);
    }
}

bool DevMem::configure()
{
    if (!valid_) {
        if (error_.empty()) {
            error_ = "dev_mem window is invalid";
        }
        return false;
    }

    auto* phal_ctx = static_cast<phal_ctx_t*>(phal_->native_context());
    if (phal_ctx == nullptr) {
        error_ = "phal context is not initialized";
        log_error("dev_mem configure failed: " + error_);
        valid_ = false;
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
    log_debug("dev_mem configure " +
              phal_aperture_call_log("phal_pcie_aperture_set",
                                     phal_ctx,
                                     spec_.dmem_bar_index,
                                     spec_.aperture_index,
                                     apt,
                                     status) +
              " aperture_bar_offset=" + hex_u64(spec_.aperture_bar_offset) +
              " pattern=" + (spec_.pattern.empty() ? "n/a" : spec_.pattern));
    if (status != PHAL_STATUS_OK) {
        error_ = phal_error("phal_pcie_aperture_set", status);
        log_error("dev_mem configure aperture_set failed: " + error_);
        valid_ = false;
        return false;
    }

    phal_pcie_aperture_t actual = {};
    status = phal_pcie_aperture_get(phal_ctx,
                                    spec_.dmem_bar_index,
                                    spec_.aperture_index,
                                    &actual);
    log_debug("dev_mem configure " +
              phal_aperture_call_log("phal_pcie_aperture_get",
                                     phal_ctx,
                                     spec_.dmem_bar_index,
                                     spec_.aperture_index,
                                     actual,
                                     status) +
              " aperture_bar_offset=" + hex_u64(spec_.aperture_bar_offset) +
              " pattern=" + (spec_.pattern.empty() ? "n/a" : spec_.pattern));
    if (status != PHAL_STATUS_OK) {
        error_ = phal_error("phal_pcie_aperture_get", status);
        log_error("dev_mem configure aperture_get failed: " + error_);
        valid_ = false;
        return false;
    }

    if (actual.identity != spec_.identity ||
        actual.target_addr != spec_.target_addr ||
        actual.size != spec_.size) {
        std::ostringstream details;
        details << "aperture get mismatch expected(identity="
                << static_cast<uint32_t>(spec_.identity)
                << ", target_addr=" << hex_u64(spec_.target_addr)
                << ", size=" << hex_u64(spec_.size)
                << ") actual(identity=" << static_cast<uint32_t>(actual.identity)
                << ", target_addr=" << hex_u64(actual.target_addr)
                << ", size=" << hex_u64(actual.size) << ")";
        error_ = details.str();
        log_error("dev_mem configure failed: " + error_);
        valid_ = false;
        return false;
    }

    return true;
}

bool DevMem::read(uint64_t offset, void* data, size_t len) const
{
    if (data == nullptr || !range_ok(offset, len)) {
        log_debug("dev_mem read " + devmem_log_context(spec_, offset, len, "invalid_range"));
        return false;
    }

    auto ok = common::bar::read(ctx_,
                                spec_.dmem_bar_index,
                                spec_.aperture_bar_offset + offset,
                                data,
                                len);
    log_debug("dev_mem read " + devmem_log_context(spec_, offset, len, ok ? "ok" : "failed"));
    return ok;
}

bool DevMem::write(uint64_t offset, const void* data, size_t len) const
{
    if (data == nullptr || spec_.readonly || !range_ok(offset, len)) {
        log_debug("dev_mem write " + devmem_log_context(spec_, offset, len, "invalid_range"));
        return false;
    }

    auto ok = common::bar::write(ctx_,
                                 spec_.dmem_bar_index,
                                 spec_.aperture_bar_offset + offset,
                                 data,
                                 len);
    log_debug("dev_mem write " + devmem_log_context(spec_, offset, len, ok ? "ok" : "failed"));
    return ok;
}
