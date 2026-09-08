#include "diag/module/PCIeModule.h"

#include <utility>

PCIeModule::PCIeModule(const std::string& name,
                       const DeviceContext& ctx,
                       const ModuleInstanceConfig& config,
                       std::unique_ptr<PCIeImpl> impl)
    : BaseDevice(name, ctx),
      bar_index_(config.bar_index),
      reg_base_offset_(config.reg_base_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    _add_test("pcie_link_status_get", [this](TestInfo& ti) { return pcie_link_status_get(ti); });
    _add_test("pcie_bar_read32", [this](TestInfo& ti) { return pcie_bar_read32(ti); });
    _add_test("pcie_bar_scan32", [this](TestInfo& ti) { return pcie_bar_scan32(ti); });
    _add_test("sequential_aperture_mapping", [this](TestInfo& ti) { return sequential_aperture_mapping(ti); });
    _add_test("pcie_dma_data_transfer", [this](TestInfo& ti) { return pcie_dma_data_transfer(ti); });
}

// Small PCIe diagnostic primitives live with the module registration. Complex
// aperture and DMA workflows remain in their dedicated testcase-family files.
TestResult PCIeModule::pcie_link_status_get(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }

    auto status = impl_->link_status_get();
    TestMetrics metrics = status.metrics;
    metrics["current_speed"] = status.current_speed;
    metrics["current_width"] = status.current_width;
    return {
        "pcie_link_status_get",
        get_name(),
        status.ok,
        metrics,
        status.ok ? "" : status.error
    };
}

TestResult PCIeModule::pcie_bar_read32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_read32(ti);
}

TestResult PCIeModule::pcie_bar_scan32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_scan32(ti);
}
