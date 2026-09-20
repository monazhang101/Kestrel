#include "diag/device/TPUDevice.h"

#include <iostream>
#include <utility>

namespace {

ModuleImplContext make_impl_context(const std::string& target_name,
                                    const DeviceContext& ctx,
                                    uint32_t index,
                                    uint32_t parent_index,
                                    uint32_t bar_index,
                                    uint64_t reg_base_offset,
                                    uint64_t reg_size)
{
    return {
        target_name,
        ctx,
        index,
        parent_index,
        bar_index,
        reg_base_offset,
        reg_size
    };
}

void print_child_tree(const TestTarget& target, const std::string& prefix)
{
    const auto children = target.child_targets();
    for (size_t i = 0; i < children.size(); ++i) {
        const auto* child = children[i];
        if (child == nullptr) {
            continue;
        }

        const bool is_last = (i + 1 == children.size());
        std::cout << prefix
                  << (is_last ? "`-- " : "|-- ")
                  << child->get_name()
                  << std::endl;

        print_child_tree(*child, prefix + (is_last ? "    " : "|   "));
    }
}

void print_bar_map_status(const DeviceContext& ctx)
{
    if (ctx.bar_mappings.empty()) {
        return;
    }

    std::cout << "  BAR mappings:" << std::endl;
    for (const auto& bar : ctx.bar_mappings) {
        std::cout << "    " << bar.name
                  << " [" << (bar.mapped ? "ok" : "fail") << "]" << std::endl;
        std::cout << "      expected_size = 0x" << std::hex << bar.expected_size << std::endl
                  << "      resource_size = 0x" << bar.resource_size << std::endl
                  << "      mapped_size   = 0x" << bar.mapped_size << std::endl;
        if (bar.mapped) {
            std::cout << "      device_base   = 0x" << bar.device_base << std::endl;
        }
        std::cout << std::dec;
        if (!bar.layout.empty()) {
            std::cout << "      layout:" << std::endl;
            for (const auto& item : bar.layout) {
                std::cout << "        - " << item << std::endl;
            }
        }
        if (!bar.error.empty()) {
            std::cout << "      error         = " << bar.error << std::endl;
        }
    }
}

}

TPUDevice::TPUDevice(const std::string& logical_name,
                     const DeviceContext& ctx,
                     const TPUDeviceConfig& config,
                     std::shared_ptr<Implementer> implementer)
    : TestTarget(logical_name, "tpu", ctx),
      tpu_index_(config.tpu_index),
      config_(config),
      implementer_(std::move(implementer))
{
    // Create the policy-defined PCIe module, if this TPU type exposes one.
    if (!config_.pcie_modules.empty()) {
        const auto& pcie_config = config_.pcie_modules.front();
        auto pcie_name = "PCIE_" + std::to_string(tpu_index_) + "_" +
                         std::to_string(pcie_config.index);
        auto impl_ctx = make_impl_context(pcie_name,
                                          ctx_,
                                          pcie_config.index,
                                          tpu_index_,
                                          pcie_config.bar_index,
                                          pcie_config.reg_base_offset,
                                          pcie_config.reg_size);
        std::unique_ptr<PCIeImpl> pcie_impl;
        if (implementer_ != nullptr) {
            pcie_impl = implementer_->pcie_impl(impl_ctx);
        }
        pcie_ = std::make_unique<PCIeModule>(
            pcie_name,
            ctx_,
            pcie_config,
            std::move(pcie_impl));
    }

    // Create the policy-defined PMU module, if present on this TPU type.
    if (!config_.pmu_modules.empty()) {
        const auto& pmu_config = config_.pmu_modules.front();
        auto pmu_name = "PMU_" + std::to_string(tpu_index_) + "_" +
                        std::to_string(pmu_config.index);
        pmu_ = std::make_unique<PMUModule>(pmu_name, ctx_, pmu_config);
    }

    for (const auto& ddp_config : config_.ddp_modules) {
        auto ddp_name = "DDP_" + std::to_string(tpu_index_) + "_" +
                        std::to_string(ddp_config.index);
        ddp_modules_.push_back(std::make_unique<DDPModule>(ddp_name, ctx_, ddp_config));
    }

    // Create all policy-defined ISI modules.
    for (const auto& isi_config : config_.isi_modules) {
        auto isi_name = "ISI_" + std::to_string(tpu_index_) + "_" +
                        std::to_string(isi_config.index);
        isi_modules_.push_back(std::make_unique<ISIModule>(isi_name, ctx_, isi_config));
    }
}

ISIModule* TPUDevice::isi(size_t index) const
{
    if (index >= isi_modules_.size()) {
        return nullptr;
    }
    return isi_modules_[index].get();
}

DDPModule* TPUDevice::ddp(size_t index) const
{
    if (index >= ddp_modules_.size()) {
        return nullptr;
    }
    return ddp_modules_[index].get();
}

std::vector<TestTarget*> TPUDevice::child_targets() const
{
    std::vector<TestTarget*> children;
    if (pcie_ != nullptr) {
        children.push_back(pcie_.get());
    }
    if (pmu_ != nullptr) {
        children.push_back(pmu_.get());
    }
    for (const auto& ddp_module : ddp_modules_) {
        children.push_back(ddp_module.get());
    }
    for (const auto& isi_module : isi_modules_) {
        children.push_back(isi_module.get());
    }
    return children;
}

void TPUDevice::print_tree() const
{
    std::cout << get_name() << " [TPU]"
              << " bdf=" << ctx_.bdf
              << " vid=0x" << std::hex << ctx_.vendor_id
              << " did=0x" << ctx_.device_id
              << std::dec
              << " impl=" << (implementer_ == nullptr ? "unknown" : to_string(implementer_->tpu_type()))
              << std::endl;
    print_bar_map_status(ctx_);

    print_child_tree(*this, "");
}
