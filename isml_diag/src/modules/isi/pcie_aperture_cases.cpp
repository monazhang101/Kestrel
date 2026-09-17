#include "diag/module/ISIModule.h"

#include "diag/core/Common.h"
#include "diag/core/DevMem.h"
#include "diag/core/HalContext.h"

#include <sstream>

extern "C" {
#include <phal/components/pcie/pcie.h>
}

// PHAL integration smoke examples:
//   1. direct PHAL context and component API access;
//   2. one-shot device-memory access through common::devmem.
namespace {

std::string hex_u64(uint64_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

}

// isi_pcie_aperture_context : Direct-PHAL smoke test proving an ISI testcase
// can use the shared PCIe PHAL context without calling into PCIeModule.
TestStatus ISIModule::IsiPcieApertureContext(TestInfo& ti)
{
    if (ti.hal == nullptr) {
        if (ti.logger != nullptr) {
            ti.logger->error("HAL context is not available");
        }
        return TestStatus::ERROR;
    }

    std::string error;
    auto* phal = static_cast<phal_ctx_t*>(ti.hal->phal().get_context(
        ctx_,
        ctx_.pcie_control_bar_index,
        ctx_.pcie_control_base,
        phal_project_from_tpu_type(ctx_.tpu_type),
        &error));
    if (phal == nullptr) {
        if (ti.logger != nullptr) {
            ti.logger->error("PHAL context init failed: " + error);
        }
        return TestStatus::ERROR;
    }

    const auto bar_index = static_cast<uint32_t>(
        common::args::get_u64(ti.args, "bar_index"));
    const auto aperture_index = static_cast<uint8_t>(
        common::args::get_u64(ti.args, "aperture_index"));

    phal_pcie_aperture_t apt = {};
    auto status = phal_pcie_aperture_get(phal, bar_index, aperture_index, &apt);
    if (status != PHAL_STATUS_OK) {
        if (ti.logger != nullptr) {
            ti.logger->error("phal_pcie_aperture_get failed: status=" +
                             std::to_string(static_cast<int>(status)));
        }
        return TestStatus::ERROR;
    }

    if (ti.logger != nullptr) {
        ti.logger->info("ISI PCIe aperture context link_id=" +
                        std::to_string(link_id_) +
                        " control_bar=" + std::to_string(ctx_.pcie_control_bar_index) +
                        " context_base=" + hex_u64(ctx_.pcie_control_base) +
                        " bar=" + std::to_string(bar_index) +
                        " aperture=" + std::to_string(aperture_index) +
                        " identity=" + std::to_string(apt.identity) +
                        " target_addr=" + hex_u64(apt.target_addr) +
                        " size=" + hex_u64(apt.size));
    }
    return TestStatus::OK;
}

// isi_common_devmem_read : Common-helper smoke test proving an ISI testcase
// can read device memory without directly managing PHAL or aperture programming.
TestStatus ISIModule::IsiCommonDevMemRead(TestInfo& ti)
{
    common::devmem::Window window;
    window.bar_index = static_cast<uint32_t>(
        common::args::get_u64(ti.args, "bar_index"));
    window.aperture_index = static_cast<uint8_t>(
        common::args::get_u64(ti.args, "aperture_index"));
    window.identity = static_cast<uint8_t>(
        common::args::get_u64(ti.args, "identity"));
    window.target_addr = common::args::get_u64(ti.args, "target_addr");
    window.size = common::args::get_u64(ti.args, "aperture_size");
    window.bar_offset = common::args::get_u64(ti.args, "bar_offset");
    const auto offset = common::args::get_u64(ti.args, "offset");

    uint32_t value = 0;
    std::string error;
    if (!common::devmem::read(ti,
                              ctx_,
                              window,
                              offset,
                              &value,
                              sizeof(value),
                              &error)) {
        if (ti.logger != nullptr) {
            ti.logger->error("common device-memory read failed: " + error);
        }
        return TestStatus::ERROR;
    }

    if (ti.logger != nullptr) {
        ti.logger->info("common device-memory read link_id=" +
                        std::to_string(link_id_) +
                        " bar=" + std::to_string(window.bar_index) +
                        " aperture=" + std::to_string(window.aperture_index) +
                        " target_addr=" + hex_u64(window.target_addr) +
                        " aperture_size=" + hex_u64(window.size) +
                        " bar_offset=" + hex_u64(window.bar_offset) +
                        " offset=" + hex_u64(offset) +
                        " value=" + hex_u64(value));
    }
    return TestStatus::OK;
}
