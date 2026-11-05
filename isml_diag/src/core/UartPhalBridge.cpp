#include "PhalContextState.h"
#include "diag/core/Format.h"

#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace {
phal_status_t status_of(const UartResponse& reply)
{
    switch (reply.result) {
    case UartResult::Ok: return PHAL_STATUS_OK;
    case UartResult::Timeout: return PHAL_STATUS_TIMEOUT;
    case UartResult::Invalid: return PHAL_STATUS_INVALID;
    case UartResult::Firmware:
        // Wire INVALID=1, TIMEOUT=2, UNKNOWN=4, BUSY=8 differ from PHAL.
        if (reply.firmware_status == 1) return PHAL_STATUS_INVALID;
        if (reply.firmware_status == 2) return PHAL_STATUS_TIMEOUT;
        return PHAL_STATUS_ERROR;
    default: return PHAL_STATUS_ERROR;
    }
}
phal_status_t transfer(env_t* env, uintptr_t addr, uint32_t* value, bool writing)
{
    if (!env || !env->priv || !value || addr > std::numeric_limits<uintptr_t>::max() - env->base)
        return PHAL_STATUS_INVALID;
    auto* io = static_cast<IhalIO*>(env->priv);
    if (!io->uart) return PHAL_STATUS_ERROR;
    const auto address = uint64_t(env->base) + addr;
    const auto reply = writing ? io->uart->write(address, *value) : io->uart->read(address);
    const auto status = status_of(reply);
    if (!writing && status == PHAL_STATUS_OK) *value = reply.value;
    auto* logger = io->device_ctx ? io->device_ctx->logger : nullptr;
    if (logger) {
        const auto text = std::string(writing ? "uart_write" : "uart_read") +
            " addr=" + common::format::hex(address) +
            " value=" + common::format::hex(writing ? *value : reply.value, 8) +
            " status=" + test_status_name(status);
        if (status != PHAL_STATUS_OK) logger->error(text + ": " + reply.error);
        else if (io->regio_suppression_depth.load(std::memory_order_relaxed) == 0)
            logger->regio(text);
    }
    return status;
}
phal_status_t read32(env_t* env, uintptr_t addr, uint32_t* value) { return transfer(env, addr, value, false); }
phal_status_t write32(env_t* env, uintptr_t addr, uint32_t value) { return transfer(env, addr, &value, true); }
phal_status_t delay_us(env_t*, uint32_t n) { std::this_thread::sleep_for(std::chrono::microseconds(n)); return PHAL_STATUS_OK; }
phal_status_t delay_ns(env_t*, uint32_t n) { std::this_thread::sleep_for(std::chrono::nanoseconds(n)); return PHAL_STATUS_OK; }
const hw_ops_t ops = [] {
    hw_ops_t value{};
    value.read = read32; value.write = write32;
    value.delay_us = delay_us; value.delay_ns = delay_ns;
    return value;
}();
}

PhalBridge::ScopedContext PhalBridge::create_uart_context(
    const DeviceContext& ctx, PhalProject project, std::string* error,
    std::shared_ptr<UartBridge> uart)
{
    if (error) error->clear();
    if (!uart) {
        if (error) *error = "UART session must be opened by isml_uart";
        return {};
    }
    auto state = std::make_unique<ScopedContext::State>();
    state->device_ctx = ctx;
    state->logger.set_level(ctx.logger ? ctx.logger->get_level() : LogLevel::Info);
    state->device_ctx.logger = &state->logger;
    state->ihalIO.device_ctx = &state->device_ctx;
    state->ihalIO.uart = std::move(uart);
    if (project == PhalProject::Generic) project = phal_project_from_tpu_type(ctx.tpu_type);
    phal_config_t config{};
    config.env_config.base = 0;
    config.env_config.user_data = &state->ihalIO;
    config.env_config.hw_ops = &ops;
    config.project_config = project == PhalProject::Atlas ? PHAL_PROJECT_ATLAS :
        project == PhalProject::AtlasM ? PHAL_PROJECT_ATLAS_M : PHAL_PROJECT_GENERIC;
    const auto status = phal_init(&state->ctx, &config);
    if (status != PHAL_STATUS_OK) {
        if (error) *error = "UART phal_init status=" + test_status_name(status);
        return {};
    }
    state->initialized = true;
    state->ctx.env.base = 0;
    return ScopedContext(std::move(state));
}
