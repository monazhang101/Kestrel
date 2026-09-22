/*
 * Common device-memory access over the PCIe BAR path.
 *
 * PHAL programs the PCIe aperture through the control BAR. The payload then
 * moves through the selected data BAR mapping; there is no direct DF MMIO
 * path behind this API.
 */
#include "diag/core/DevMem.h"

#include "diag/core/Common.h"
#include "diag/core/Format.h"
#include "diag/core/HalContext.h"

#include <limits>
#include <sstream>

using common::format::hex;
using common::format::hex_bytes;

extern "C" {
#include <phal/components/pcie/pcie.h>
}

namespace common::devmem {
namespace {

bool fail(std::string* error, const std::string& message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool prepare_window(phal_ctx_t* phal, Logger* logger,
                    const DeviceContext& ctx,
                    const Window& window,
                    uint64_t offset,
                    size_t len,
                    std::string* error)
{
    if (phal == nullptr) return fail(error, "PCIe PHAL context is not initialized");
    if (!common::mmio::is_valid_range(window.size, offset, len)) {
        return fail(error, "device-memory access is outside the aperture window");
    }
    if (window.bar_offset > std::numeric_limits<uint64_t>::max() - offset) {
        return fail(error, "device-memory BAR offset overflow");
    }
    if (window.size == 0 || window.target_addr >
        std::numeric_limits<uint64_t>::max() - (window.size - 1)) {
        return fail(error, "device-memory target range overflow");
    }
    const auto absolute_bar_offset = window.bar_offset + offset;
    const auto* data_bar = common::bar::find(ctx, window.bar_index);
    if (data_bar == nullptr || !data_bar->mapped ||
        !common::mmio::is_valid_range(data_bar->mapped_size,
                                      absolute_bar_offset,
                                      len)) {
        return fail(error, "device-memory access is outside the mapped data BAR");
    }

    phal_pcie_aperture_t requested = {};
    requested.identity = window.identity;
    requested.target_addr = window.target_addr;
    requested.size = window.size;

    auto status = phal_pcie_aperture_set(phal,
                                         window.bar_index,
                                         window.aperture_index,
                                         &requested);
    if (status != PHAL_STATUS_OK) {
        return fail(error,
                    "phal_pcie_aperture_set failed: status=" +
                        std::to_string(static_cast<int>(status)));
    }

    phal_pcie_aperture_t actual = {};
    status = phal_pcie_aperture_get(phal,
                                    window.bar_index,
                                    window.aperture_index,
                                    &actual);
    if (status != PHAL_STATUS_OK) {
        return fail(error,
                    "phal_pcie_aperture_get failed: status=" +
                        std::to_string(static_cast<int>(status)));
    }
    if (actual.identity != requested.identity ||
        actual.target_addr != requested.target_addr ||
        actual.size != requested.size) {
        return fail(error, "PCIe aperture readback does not match the requested window");
    }

    if (logger != nullptr) {
        std::ostringstream stream;
        stream << "Device-memory aperture configured"
               << "\n       bar_index        = " << window.bar_index
               << "\n       aperture_index   = "
               << static_cast<uint32_t>(window.aperture_index)
               << "\n       target_addr      = " << hex(window.target_addr)
               << "\n       window_size      = " << hex(window.size)
               << "\n       data_bar_offset  = " << hex(window.bar_offset);
        logger->debug(stream.str());
    }
    return true;
}


bool transfer(phal_ctx_t* phal, Logger* logger, const DeviceContext& ctx,
              const Window& window, uint64_t offset, void* output,
              const void* input, size_t len, std::string* error)
{
    if (error != nullptr) error->clear();
    if (input == nullptr && output == nullptr) return fail(error, "device-memory buffer is null");
    const bool writing = input != nullptr;
    const auto* bar = common::bar::find(ctx, window.bar_index);
    if (writing && (bar == nullptr || !bar->mapped || !bar->writable))
        return fail(error, "device-memory data BAR is not writable");
    if (!prepare_window(phal, logger, ctx, window, offset, len, error)) return false;
    const bool ok = writing
        ? common::bar::write(ctx, window.bar_index, window.bar_offset + offset, input, len)
        : common::bar::read(ctx, window.bar_index, window.bar_offset + offset, output, len);
    if (!ok) return fail(error, "device-memory BAR access failed");
    if (logger != nullptr) {
        logger->debug(std::string(writing ? "D-MEM write" : "D-MEM read") +
            " target_addr=" + hex(window.target_addr + offset) +
            " size_bytes=" + std::to_string(len) + " data_preview=" +
            hex_bytes(writing ? input : output, len));
    }
    return true;
}

phal_ctx_t* pcie_context(TestInfo& ti, const DeviceContext& ctx, std::string* error)
{
    if (ti.hal == nullptr) {
        fail(error, "HAL context is not available");
        return nullptr;
    }
    return ti.hal->phal().get_context(
        ctx, ctx.pcie_control_base,
        phal_project_from_tpu_type(ctx.tpu_type), error);
}
}

// Existing configurable/buffer API retained for callers choosing their window.
bool read(TestInfo& ti, const DeviceContext& ctx, const Window& window,
          uint64_t offset, void* data, size_t len, std::string* error)
{
    auto* phal = pcie_context(ti, ctx, error);
    return phal != nullptr && transfer(phal, ti.logger, ctx, window, offset,
                                       data, nullptr, len, error);
}

bool write(TestInfo& ti, const DeviceContext& ctx, const Window& window,
           uint64_t offset, const void* data, size_t len, std::string* error)
{
    auto* phal = pcie_context(ti, ctx, error);
    return phal != nullptr && transfer(phal, ti.logger, ctx, window, offset,
                                       nullptr, data, len, error);
}
}

namespace common {
namespace {
TestStatus dmem_transfer(DeviceContext& ctx, uint64_t addr, void* output,
                         const void* input, size_t len, size_t alignment)
{
    if ((output == nullptr && input == nullptr) || addr % alignment != 0 ||
        ctx.dmem_base % alignment != 0 || !mmio::is_valid_range(ctx.dmem_size, addr, len) ||
        ctx.dmem_base > std::numeric_limits<uint64_t>::max() - (ctx.dmem_size - 1)) {
        if (ctx.logger) ctx.logger->error("invalid D-MEM address or buffer length");
        return PHAL_STATUS_INVALID;
    }
    // One framework scratch window. Never add the register block offset here.
    const devmem::Window window{4, 0, 0, ctx.dmem_base, ctx.dmem_size, 0};
    std::string error;
    const bool ok = devmem::transfer(ctx.pcie_phal, ctx.logger,
        ctx, window, addr, output, input, len, &error);
    if (!ok) {
        if (ctx.logger) ctx.logger->error("D-MEM access failed: " + error);
        return PHAL_STATUS_ERROR;
    }
    if (ctx.logger) ctx.logger->info(std::string(input ? "dmem_write" : "dmem_read") +
        " target=" + ctx.target_name + " address=" + hex(ctx.dmem_base + addr) +
        " size_bytes=" + std::to_string(len) +
        " data_preview=" + hex_bytes(input ? input : output, len));
    return PHAL_STATUS_OK;
}
}

TestStatus dmem_read(DeviceContext& ctx, uint64_t addr, uint32_t* data)
{
    return dmem_transfer(ctx, addr, data, nullptr, sizeof(uint32_t), alignof(uint32_t));
}

TestStatus dmem_write(DeviceContext& ctx, uint64_t addr, uint32_t data)
{
    return dmem_transfer(ctx, addr, nullptr, &data, sizeof(data), alignof(uint32_t));
}

TestStatus dmem_read(DeviceContext& ctx, uint64_t addr, std::vector<uint8_t>* data)
{
    return dmem_transfer(ctx, addr, data ? data->data() : nullptr, nullptr,
                         data ? data->size() : 0, 1);
}

TestStatus dmem_write(DeviceContext& ctx, uint64_t addr, const std::vector<uint8_t>& data)
{
    return dmem_transfer(ctx, addr, nullptr, data.data(), data.size(), 1);
}
}
