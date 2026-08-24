#include "diag/module/PCIeModule.h"

#include <utility>

PCIeModule::PCIeModule(const std::string& name,
                       const DeviceContext& ctx,
                       const ModuleInstanceConfig& config,
                       std::unique_ptr<PCIeImpl> impl)
    : BaseDevice(name, ctx),
      reg_offset_(config.reg_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    // ----------------- Atomic Tests -----------------
    _add_test("pcie_link_status_get", [this](TestInfo& ti) { return PcieLinkStatusGet(ti); });
    _add_test("pcie_dma_data_transfer", [this](TestInfo& ti) { return PcieDmaDataTransfer(ti); });

}

// pcie_link_status_get : To get the current PCIe negotiated speed and link width.
// @input: none
// @output: TestResult metrics include current_speed and current_width.
TestResult PCIeModule::PcieLinkStatusGet(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->PcieLinkStatusGet(ti);
}

// pcie_dma_data_transfer : To run PCIe DMA data movement and compare the transferred pattern.
// @input: "direction": h2d/d2h, "size_bytes", "pattern"
// @output: TestResult metrics include direction, size_bytes, pattern, data_compare.
TestResult PCIeModule::PcieDmaDataTransfer(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->PcieDmaDataTransfer(ti);
}
