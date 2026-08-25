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

    _add_test("pcie_link_status_get", [this](TestInfo& ti) { return pcie_link_status_get(ti); });
    _add_test("pcie_dma_data_transfer", [this](TestInfo& ti) { return pcie_dma_data_transfer(ti); });
}
