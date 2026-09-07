/*
 * Common device-memory access over the PCIe BAR path.
 *
 * PHAL programs the PCIe aperture through the control BAR. The payload then
 * moves through the selected data BAR mapping; there is no direct DF MMIO
 * path behind this API.
 */
#include "diag/core/DevMem.h"

#include "diag/core/Common.h"
#include "diag/core/HalContext.h"

#include <limits>
#include <sstream>

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

std::string hex_u64(uint64_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

bool prepare_window(TestInfo& ti,
                    const DeviceContext& ctx,
                    const Window& window,
                    uint64_t offset,
                    size_t len,
                    std::string* error)
{
    if (ti.hal == nullptr) {
        return fail(error, "HAL context is not available");
    }
    if (!common::mmio::is_valid_range(window.size, offset, len)) {
        return fail(error, "device-memory access is outside the aperture window");
    }
    if (window.bar_offset > std::numeric_limits<uint64_t>::max() - offset) {
        return fail(error, "device-memory BAR offset overflow");
    }
    const auto absolute_bar_offset = window.bar_offset + offset;
    const auto* data_bar = common::bar::find(ctx, window.bar_index);
    if (data_bar == nullptr || !data_bar->mapped ||
        !common::mmio::is_valid_range(data_bar->mapped_size,
                                      absolute_bar_offset,
                                      len)) {
        return fail(error, "device-memory access is outside the mapped data BAR");
    }

    std::string phal_error;
    auto* phal = static_cast<phal_ctx_t*>(ti.hal->phal().get_context(
        ctx,
        ctx.pcie_control_bar_index,
        ctx.pcie_control_base,
        phal_project_from_tpu_type(ctx.tpu_type),
        &phal_error));
    if (phal == nullptr) {
        return fail(error, "PHAL context init failed: " + phal_error);
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

    if (ti.logger != nullptr) {
        std::ostringstream stream;
        stream << "devmem aperture configured"
               << " bar=" << window.bar_index
               << " aperture=" << static_cast<uint32_t>(window.aperture_index)
               << " target=" << hex_u64(window.target_addr)
               << " size=" << hex_u64(window.size)
               << " bar_offset=" << hex_u64(window.bar_offset);
        ti.logger->debug(stream.str());
    }
    return true;
}

}

bool read(TestInfo& ti,
          const DeviceContext& ctx,
          const Window& window,
          uint64_t offset,
          void* data,
          size_t len,
          std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (data == nullptr) {
        return fail(error, "device-memory read buffer is null");
    }
    if (!prepare_window(ti, ctx, window, offset, len, error)) {
        return false;
    }
    if (!common::bar::read(ctx,
                           window.bar_index,
                           window.bar_offset + offset,
                           data,
                           len)) {
        return fail(error, "device-memory BAR read failed");
    }
    return true;
}

bool write(TestInfo& ti,
           const DeviceContext& ctx,
           const Window& window,
           uint64_t offset,
           const void* data,
           size_t len,
           std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (data == nullptr) {
        return fail(error, "device-memory write buffer is null");
    }
    if (!prepare_window(ti, ctx, window, offset, len, error)) {
        return false;
    }
    if (!common::bar::write(ctx,
                            window.bar_index,
                            window.bar_offset + offset,
                            data,
                            len)) {
        return fail(error, "device-memory BAR write failed");
    }
    return true;
}

}
