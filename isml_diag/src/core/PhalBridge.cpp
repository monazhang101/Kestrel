#include "PhalContextState.h"

#include "diag/core/Common.h"

#include <chrono>
#include <limits>
#include <sstream>
#include <thread>
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
    if (logger == nullptr) return;
    if (status == PHAL_STATUS_OK &&
        (logger->get_level() < LogLevel::RegIo ||
         io->regio_suppression_depth.load(std::memory_order_relaxed) != 0)) return;
    using common::format::hex;
    const auto message = std::string(operation) + " bdf=" + io->device_ctx->bdf +
        " env_base=" + hex(env->base) + " offset=" + hex(addr) +
        " bar0_offset=" + hex(env->base + addr) + " value=" + hex(data, 8) +
        " status=" + std::to_string(static_cast<int>(status));
    if (status == PHAL_STATUS_OK) logger->regio(message);
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

// Host scheduler delays may exceed the requested duration; these callbacks
// do not provide precise hardware timing or MMIO completion guarantees.
static phal_status_t hal_drv_delay_us(env_t* env, uint32_t count)
{
    if (env == nullptr) return PHAL_STATUS_INVALID;
    std::this_thread::sleep_for(std::chrono::microseconds(count));
    return PHAL_STATUS_OK;
}

static phal_status_t hal_drv_delay_ns(env_t* env, uint32_t count)
{
    if (env == nullptr) return PHAL_STATUS_INVALID;
    // Keep nanosecond units so sub-microsecond requests are not truncated to 0.
    std::this_thread::sleep_for(std::chrono::nanoseconds(count));
    return PHAL_STATUS_OK;
}

hw_ops_t make_hal_drv_ops()
{
    hw_ops_t ops{};
    ops.write = hal_drv_write;
    ops.read = hal_drv_read;
    ops.delay_us = hal_drv_delay_us;
    ops.delay_ns = hal_drv_delay_ns;
    return ops;
}

const hw_ops_t hal_drv_ops = make_hal_drv_ops();

// ---------------------------------------------------------------------------
// IPC ops: phal_read/phal_write forwarded to the firmware through
// the host<->fw IPC slots (see IpcBridge). The env->priv pointer is
// the IhalIO that owns the bound IpcBridge.
// ---------------------------------------------------------------------------
static phal_status_t to_phal_status(IpcStatus status);

/*
 * Human-readable per-fw reason for an IPC-path status, appended to the
 * ipc_read/ipc_write log lines. phal_status_t collapses the IPC codes
 * (ErrInvalid and ErrUnknown both fold into PHAL_STATUS_*), so the numeric
 * status alone cannot tell "bad request" from "no handler"; this string
 * restores the distinction. The input is the PHAL-layer status carried in
 * the log line, so map it back to IpcStatus first.
 */
static std::string ipc_status_text(IpcFwId fw, phal_status_t status);

/*
 * Log one IPC-path phal_read/phal_write with the fields the phal_* lines
 * cannot express. The phal_* log derives env_base/offset/bar0_offset from
 * the MMIO path (env->base + addr); for IPC those are meaningless - the
 * actual MMIO traffic is the seq/ack slot poll, and addr is a firmware-
 * side address carried in the slot payload. Log the real placement:
 *   env_base    = IPC region's BAR0-window base (RCF base + region base)
 *   offset      = slot's offset within the region (IPC_CORE_STRIDE*core_id)
 *   bar0_offset = the polled slot's absolute BAR0-window address
 *   fw_addr     = firmware-side address carried in the request
 *   value       = raw slot value (the firmware's error code on failure)
 */
static void log_ipc_io(const char* operation, const IhalIO* io,
                       uintptr_t fw_addr, uint32_t value,
                       phal_status_t status)
{
    if (io == nullptr || io->device_ctx == nullptr) return;
    auto* logger = io->device_ctx->logger;
    if (logger == nullptr) return;
    if (status == PHAL_STATUS_OK &&
        (logger->get_level() < LogLevel::RegIo ||
         io->regio_suppression_depth.load(std::memory_order_relaxed) != 0)) return;
    using common::format::hex;
    const auto& ipc = io->ipc;
    const auto message = std::string(operation) + " bdf=" +
        io->device_ctx->bdf +
        " env_base=" + hex(ipc.region_bar0_base()) +
        " offset=" + hex(ipc.slot_region_offset()) +
        " bar0_offset=" + hex(ipc.slot_bar0_offset()) +
        " fw_addr=" + hex(static_cast<uint64_t>(fw_addr), 8) +
        " value=" + hex(value, 8) +
        " status=" + std::to_string(static_cast<int>(status)) +
        " (" + ipc_status_text(io->fw, status) + ")";
    if (status == PHAL_STATUS_OK) logger->regio(message);
    else logger->error(message);
}

static phal_status_t to_phal_status(IpcStatus status)
{
    switch (status) {
    case IpcStatus::Ok: return PHAL_STATUS_OK;
    case IpcStatus::ErrTimeout: return PHAL_STATUS_TIMEOUT;
    case IpcStatus::ErrInvalid: return PHAL_STATUS_INVALID;
    default: return PHAL_STATUS_ERROR;
    }
}

/*
 * Human-readable per-fw reason for an IPC-path status, appended to the
 * ipc_read/ipc_write log lines. phal_status_t collapses the IPC codes
 * (ErrInvalid and ErrUnknown both fold into PHAL_STATUS_*), so the numeric
 * status alone cannot tell "bad request" from "no handler"; this string
 * restores the distinction. The input is the PHAL-layer status carried in
 * the log line, so map it back to IpcStatus first.
 */
static std::string ipc_status_text(IpcFwId fw, phal_status_t status)
{
    IpcStatus ipc_status;
    switch (status) {
    case PHAL_STATUS_TIMEOUT:
        ipc_status = IpcStatus::ErrTimeout;
        break;
    case PHAL_STATUS_INVALID:
        ipc_status = IpcStatus::ErrInvalid;
        break;
    default:
        // PHAL_STATUS_OK and PHAL_STATUS_ERROR both mean "no IPC error"
        // here: OK is success, ERROR is the fold-through of ErrUnknown.
        ipc_status = IpcStatus::Ok;
        break;
    }
    std::string text;
    to_ipc_status_text(ipc_status, fw, text);
    return text;
}

static phal_status_t ipc_drv_write(env_t* env, uintptr_t addr, uint32_t data)
{
    if (env == nullptr || env->priv == nullptr) {
        return PHAL_STATUS_INVALID;
    }
    auto* io = static_cast<IhalIO*>(env->priv);
    const auto status = static_cast<IpcStatus>(
        io->ipc.write(static_cast<uint32_t>(addr), data));
    log_ipc_io("ipc_write", io, addr, data, to_phal_status(status));
    return to_phal_status(status);
}

static phal_status_t ipc_drv_read(env_t* env, uintptr_t addr, uint32_t* data)
{
    if (env == nullptr || env->priv == nullptr || data == nullptr) {
        return PHAL_STATUS_INVALID;
    }
    auto* io = static_cast<IhalIO*>(env->priv);
    uint32_t value = 0;
    const auto status = io->ipc.read(static_cast<uint32_t>(addr), value);
    if (status == IpcStatus::Ok) *data = value;
    // value carries the firmware's error code when status != Ok, so it is
    // logged either way (read() only overwrites *data on success).
    log_ipc_io("ipc_read", io, addr, value, to_phal_status(status));
    return to_phal_status(status);
}

hw_ops_t make_ipc_drv_ops()
{
    // IPC shares the delay semantics with the direct MMIO path.
    hw_ops_t ops{};
    ops.write = ipc_drv_write;
    ops.read = ipc_drv_read;
    ops.delay_us = hal_drv_delay_us;
    ops.delay_ns = hal_drv_delay_ns;
    return ops;
}

const hw_ops_t ipc_drv_ops = make_ipc_drv_ops();

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

PhalBridge::ScopedContext PhalBridge::create_ipc_context(const DeviceContext& ctx,
                                                          PhalProject project,
                                                          IpcFwId fw,
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
    state->ihalIO.fw = fw;

    // Map the fw's IPC slots out of the same BAR0 window before the ops
    // can touch them.
    std::string ipc_error;
    if (!state->ihalIO.ipc.bind(ctx, fw, &ipc_error)) {
        if (error != nullptr) {
            *error = "IPC bind failed (fw=" + std::string(to_string(fw)) +
                     "): " + ipc_error;
        }
        return {};
    }

    phal_config_t config{};
    config.env_config.base = CHIP_RCF_BASE;
    config.env_config.user_data = &state->ihalIO;
#if !defined(HAL_FIRMWARE_MODE)
    config.env_config.hw_ops = &ipc_drv_ops;
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
        }
        return {};
    }

    state->initialized = true;
    state->ctx.env.base = CHIP_RCF_BASE;
    return ScopedContext(std::move(state));
}
