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
TestResult ISIModule::IsiPcieApertureContext(TestInfo& ti)
{
    TestMetrics metrics = {
        {"link_id", std::to_string(link_id_)},
        {"control_bar", std::to_string(ctx_.pcie_control_bar_index)},
        {"context_base", hex_u64(ctx_.pcie_control_base)},
    };

    auto make_result = [&](bool passed,
                           const std::string& error_description = {},
                           const std::string& error_details = {}) {
        return TestResult{
            ti.test_name,
            ti.target_name,
            passed,
            metrics,
            error_description,
            error_details
        };
    };

    if (ti.hal == nullptr) {
        return make_result(false, "HAL context is not available");
    }

    std::string error;
    auto* phal = static_cast<phal_ctx_t*>(ti.hal->phal().get_context(
        ctx_,
        ctx_.pcie_control_bar_index,
        ctx_.pcie_control_base,
        phal_project_from_tpu_type(ctx_.tpu_type),
        &error));
    if (phal == nullptr) {
        return make_result(false, "PHAL context init failed", error);
    }

    const auto bar_index = static_cast<uint32_t>(
        common::args::get_u64(ti.args, "bar_index", 4));
    const auto aperture_index = static_cast<uint8_t>(
        common::args::get_u64(ti.args, "aperture_index", 0));

    phal_pcie_aperture_t apt = {};
    auto status = phal_pcie_aperture_get(phal, bar_index, aperture_index, &apt);
    metrics["bar_index"] = std::to_string(bar_index);
    metrics["aperture_index"] = std::to_string(static_cast<uint32_t>(aperture_index));
    metrics["phal_status"] = std::to_string(static_cast<int>(status));

    if (status != PHAL_STATUS_OK) {
        return make_result(false,
                           "phal_pcie_aperture_get failed",
                           "status=" + std::to_string(static_cast<int>(status)));
    }

    metrics["identity"] = std::to_string(static_cast<uint32_t>(apt.identity));
    metrics["target_addr"] = hex_u64(apt.target_addr);
    metrics["size"] = hex_u64(apt.size);
    return make_result(true);
}

// isi_common_devmem_read : Common-helper smoke test proving an ISI testcase
// can read device memory without directly managing PHAL or aperture programming.
TestResult ISIModule::IsiCommonDevMemRead(TestInfo& ti)
{
    common::devmem::Window window;
    window.bar_index = static_cast<uint32_t>(
        common::args::get_u64(ti.args, "bar_index", 4));
    window.aperture_index = static_cast<uint8_t>(
        common::args::get_u64(ti.args, "aperture_index", 0));
    window.identity = static_cast<uint8_t>(
        common::args::get_u64(ti.args, "identity", 0));
    window.target_addr = common::args::get_u64(ti.args, "target_addr", 0x10000000);
    window.size = common::args::get_u64(ti.args, "aperture_size", 0x100000);
    window.bar_offset = common::args::get_u64(ti.args, "bar_offset", 0);
    const auto offset = common::args::get_u64(ti.args, "offset", 0);

    TestMetrics metrics = {
        {"access_path", "common::devmem"},
        {"link_id", std::to_string(link_id_)},
        {"bar_index", std::to_string(window.bar_index)},
        {"aperture_index", std::to_string(window.aperture_index)},
        {"target_addr", hex_u64(window.target_addr)},
        {"aperture_size", hex_u64(window.size)},
        {"bar_offset", hex_u64(window.bar_offset)},
        {"offset", hex_u64(offset)},
    };

    uint32_t value = 0;
    std::string error;
    const auto passed = common::devmem::read(ti,
                                              ctx_,
                                              window,
                                              offset,
                                              &value,
                                              sizeof(value),
                                              &error);
    if (passed) {
        metrics["value"] = hex_u64(value);
    }

    return {
        ti.test_name,
        ti.target_name,
        passed,
        metrics,
        passed ? "" : "common device-memory read failed",
        passed ? "" : error
    };
}
