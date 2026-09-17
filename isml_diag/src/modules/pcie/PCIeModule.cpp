#include "diag/modules/PCIeModule.h"

#include <utility>

PCIeModule::PCIeModule(const std::string& name,
                       const DeviceContext& ctx,
                       const ModuleInstanceConfig& config,
                       std::unique_ptr<PCIeImpl> impl)
    : TestTarget(name, "pcie", ctx),
      bar_index_(config.bar_index),
      reg_base_offset_(config.reg_base_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    _add_test("bar_read32",
              {{"offset", "0", "offset"}},
              [this](TestInfo& ti) { return bar_read32(ti); });
    _add_test("bar_scan32",
              {{"offset", "0", "offset"},
               {"words", "16", "u64"}},
              [this](TestInfo& ti) { return bar_scan32(ti); });
    _add_test("sequential_aperture_mapping", {},
              [this](TestInfo& ti) { return sequential_aperture_mapping(ti); });
    _add_test("dma_data_transfer",
              {{"direction", "h2d", "string"},
               {"size_bytes", "4096", "bytes"},
               {"pattern", "random", "string"},
               {"device_offset", "0x10200", "offset"},
               // Device-only round trips return to a separate DMEM region.
               {"return_offset", "0x10400", "offset"},
               {"intermediate_offset", "0", "offset"},
               {"timeout_ms", "1000", "ms"}},
              [this](TestInfo& ti) { return dma_data_transfer(ti); });
}

// Small PCIe diagnostic primitives live with the module registration. Complex
// aperture and DMA workflows remain in their dedicated testcase-family files.
TestStatus PCIeModule::bar_read32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_read32(ti);
}

TestStatus PCIeModule::bar_scan32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_scan32(ti);
}
