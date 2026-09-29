#include "diag/modules/PCIeModule.h"
#include "diag/core/Common.h"

#include <utility>

PCIeModule::PCIeModule(const std::string& name,
                       const DeviceContext& ctx,
                       const ModuleInstanceConfig& config,
                       std::unique_ptr<PCIeImpl> impl)
    : TestTarget(name, "pcie", ctx),
      impl_(std::move(impl))
{
    ctx_.module_index = config.index;
    _add_test("bar_read32",
              {{"bar_index", "0", "u32"},
               {"offset", "", "offset"}},
              [this](TestInfo& ti) { return bar_read32(ti); });
    _add_test("example", {},
              [this](TestInfo& ti) { return example(ti); });
    _add_test("sequential_aperture_mapping", {},
              [this](TestInfo& ti) { return sequential_aperture_mapping(ti); });
    // _add_test adds "pcie_": CLI name is pcie_dma_data_transfer.
    // TestTarget supplies these defaults through ti.args when flags are omitted.
    _add_test("dma_data_transfer",
              {{"direction", "h2d", "string"},
               {"size_bytes", "4096", "bytes"},
               {"pattern", "random", "string"},
               {"intermediate_offset", "0", "offset"},
               {"timeout_ms", "1000", "ms"}},
              [this](TestInfo& ti) { return dma_data_transfer(ti); });
}

// Small PCIe diagnostic primitives live with the module registration. Complex
// aperture and DMA workflows remain in their dedicated testcase-family files.
TestStatus PCIeModule::bar_read32(TestInfo& ti)
{
    const auto bar_index = common::args::get_u64(ti.args, "bar_index");
    const auto offset = common::args::get_u64(ti.args, "offset");
    if (bar_index > UINT32_MAX) {
        ti.logger->error("BAR index is outside uint32 range");
        return PHAL_STATUS_INVALID;
    }
    if (offset % sizeof(uint32_t) != 0) {
        ti.logger->error("BAR offset must be 4-byte aligned: " + common::format::hex(offset));
        return PHAL_STATUS_INVALID;
    }

    uint32_t value = 0;
    ti.logger->info("BAR read start: bar=" + std::to_string(bar_index) +
                    " offset=" + common::format::hex(offset));
    if (ti.hal == nullptr ||
        !common::bar::read32(ti.hal->device_context(),
                             static_cast<uint32_t>(bar_index), offset, value)) {
        ti.logger->error("BAR read failed: bar=" + std::to_string(bar_index) +
                         " offset=" + common::format::hex(offset));
        return PHAL_STATUS_ERROR;
    }

    ti.logger->info("BAR read completed: bar=" + std::to_string(bar_index) +
                    " offset=" + common::format::hex(offset) +
                    " value=" + common::format::hex(value, 8));
    return PHAL_STATUS_OK;
}
