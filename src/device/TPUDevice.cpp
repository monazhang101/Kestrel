#include "diag/device/TPUDevice.h"

#include "diag/core/Common.h"

#include <iostream>
#include <utility>

namespace {

ModuleImplContext make_impl_context(const std::string& target_name,
                                    const DeviceContext& ctx,
                                    uint32_t index,
                                    uint32_t parent_index,
                                    uint64_t reg_offset,
                                    uint64_t reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx.mapped_bar_base);
    return {
        target_name,
        ctx,
        index,
        parent_index,
        bar_base == nullptr ? nullptr : bar_base + reg_offset,
        reg_offset,
        reg_size
    };
}

}

TPUDevice::TPUDevice(const std::string& logical_name,
                     const DeviceContext& ctx,
                     const TPUDeviceConfig& config,
                     std::shared_ptr<Implementer> implementer)
    : BaseDevice(logical_name, ctx),
      tpu_index_(config.tpu_index),
      config_(config),
      implementer_(std::move(implementer))
{
    // Bind the top-level TPU operation implementation.
    auto tpu_impl_ctx = make_impl_context(logical_name, ctx_, tpu_index_, 0, 0, ctx_.bar_size);
    if (implementer_ != nullptr) {
        impl_ = implementer_->tpu_impl(tpu_impl_ctx);
    }

    // ----------------- Atomic Tests -----------------
    _add_test("identify", [this](TestInfo& ti) { return Identify(ti); });

    // Create the policy-defined PCIe module, if this TPU type exposes one.
    if (!config_.pcie_modules.empty()) {
        const auto& pcie_config = config_.pcie_modules.front();
        auto pcie_name = logical_name + ".PCIE_" + std::to_string(pcie_config.index);
        auto impl_ctx = make_impl_context(pcie_name,
                                          ctx_,
                                          pcie_config.index,
                                          tpu_index_,
                                          pcie_config.reg_offset,
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
        auto pmu_name = logical_name + ".PMU_" + std::to_string(pmu_config.index);
        auto impl_ctx = make_impl_context(pmu_name,
                                          ctx_,
                                          pmu_config.index,
                                          tpu_index_,
                                          pmu_config.reg_offset,
                                          pmu_config.reg_size);
        std::unique_ptr<PMUImpl> pmu_impl;
        if (implementer_ != nullptr) {
            pmu_impl = implementer_->pmu_impl(impl_ctx);
        }
        pmu_ = std::make_unique<PMUModule>(
            pmu_name,
            ctx_,
            pmu_config,
            std::move(pmu_impl));
    }

    // Create all policy-defined DDP modules and their DMC children.
    for (const auto& ddp_config : config_.ddp_modules) {
        auto ddp_name = logical_name + ".DDP_" + std::to_string(ddp_config.index);
        auto impl_ctx = make_impl_context(ddp_name,
                                          ctx_,
                                          ddp_config.index,
                                          tpu_index_,
                                          ddp_config.reg_offset,
                                          ddp_config.reg_size);
        std::unique_ptr<DDPImpl> ddp_impl;
        if (implementer_ != nullptr) {
            ddp_impl = implementer_->ddp_impl(impl_ctx);
        }
        ddp_modules_.push_back(std::make_unique<DDPModule>(
            ddp_name,
            ctx_,
            ddp_config,
            std::move(ddp_impl),
            implementer_));
    }

    // Create all policy-defined ISI modules.
    for (const auto& isi_config : config_.isi_modules) {
        auto isi_name = logical_name + ".ISI_" + std::to_string(isi_config.index);
        auto impl_ctx = make_impl_context(isi_name,
                                          ctx_,
                                          isi_config.index,
                                          tpu_index_,
                                          isi_config.reg_offset,
                                          isi_config.reg_size);
        std::unique_ptr<ISIImpl> isi_impl;
        if (implementer_ != nullptr) {
            isi_impl = implementer_->isi_impl(impl_ctx);
        }
        isi_modules_.push_back(std::make_unique<ISIModule>(
            isi_name,
            ctx_,
            isi_config,
            std::move(isi_impl)));
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

std::vector<BaseDevice*> TPUDevice::child_targets() const
{
    std::vector<BaseDevice*> children;
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
              << " bar_device_base=0x" << ctx_.bar_device_base
              << " bar_size=0x" << ctx_.bar_size
              << " impl=" << (implementer_ == nullptr ? "unknown" : to_string(implementer_->tpu_type()))
              << std::dec << std::endl;

    for (const auto* child : child_targets()) {
        std::cout << "  |-- " << child->get_name() << std::endl;
    }
}

// identify : To read ATLAS identity registers and report stable hardware facts.
// @input: none.
// @output: TestResult metrics include bdf, vendor_id, device_id, chip_id, and revision.
TestResult TPUDevice::Identify(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "TPU implementation is not bound");
    }
    return impl_->Identify(ti);
}
