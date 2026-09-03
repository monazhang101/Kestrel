#include "diag/module/PCIeModule.h"

#include "diag/core/Common.h"

#include <utility>

PCIeModule::PCIeModule(const std::string& name,
                       const DeviceContext& ctx,
                       const ModuleInstanceConfig& config,
                       std::unique_ptr<PCIeImpl> impl)
    : BaseDevice(name, ctx),
      bar_index_(config.bar_index),
      reg_offset_(config.reg_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
{
    auto* bar_base = static_cast<uint8_t*>(common::bar::mapped_base(ctx_, bar_index_));
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    _add_test("pcie_link_status_get", [this](TestInfo& ti) { return pcie_link_status_get(ti); });
    _add_test("pcie_bar_read32", [this](TestInfo& ti) { return pcie_bar_read32(ti); });
    _add_test("pcie_bar_scan32", [this](TestInfo& ti) { return pcie_bar_scan32(ti); });
    _add_test("sequential_aperture_mapping", [this](TestInfo& ti) { return sequential_aperture_mapping(ti); });
    _add_test("pcie_dma_data_transfer", [this](TestInfo& ti) { return pcie_dma_data_transfer(ti); });
}
