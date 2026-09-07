#include "generic_impl.h"

#include "diag/core/Common.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace {

std::string hex32(uint32_t value)
{
    std::ostringstream stream;
    stream << "0x"
           << std::hex
           << std::setw(8)
           << std::setfill('0')
           << value;
    return stream.str();
}

}

namespace generic_impl {

// Generic top-level TPU impl. Project-specific TPU impls inherit this block and
// override only the SoC-level operations they support.
GenericTPUImpl::GenericTPUImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestResult GenericTPUImpl::Identify(TestInfo& ti)
{
    return make_unimplemented_result(ti, "TPU identify is not implemented for " + ctx_.target_name);
}

// Generic PCIe impl. This is the fallback for link, BAR, and DMA operations when
// a product-specific PCIe implementation does not provide an override.
GenericPCIeImpl::GenericPCIeImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

LinkStatus GenericPCIeImpl::link_status_get()
{
    return {false, "", "", "PCIe link status get is not implemented for " + ctx_.target_name};
}

TestResult GenericPCIeImpl::bar_read32(TestInfo& ti)
{
    auto offset = common::args::get_u64(ti.args, "offset", 0);
    auto requested_bar = ti.args.find("bar_index");

    if ((offset % sizeof(uint32_t)) != 0) {
        return {"pcie_bar_read32", ctx_.target_name, false, {
            {"offset", std::to_string(offset)}
        }, "offset must be 4-byte aligned"};
    }

    uint32_t value = 0;
    if (requested_bar != ti.args.end()) {
        auto bar_index = static_cast<uint32_t>(
            common::args::get_u64(ti.args, "bar_index", ctx_.bar_index));
        if (!common::bar::read32(ctx_.device_ctx, bar_index, offset, value)) {
            const auto* bar = common::bar::find(ctx_.device_ctx, bar_index);
            return {"pcie_bar_read32", ctx_.target_name, false, {
                {"bar_index", std::to_string(bar_index)},
                {"offset", std::to_string(offset)},
                {"mapped_size", std::to_string(bar == nullptr ? 0 : bar->mapped_size)}
            }, "BAR read32 failed", "check BAR mmap, offset alignment, and BAR range"};
        }

        return {"pcie_bar_read32", ctx_.target_name, true, {
            {"bar_index", std::to_string(bar_index)},
            {"offset", std::to_string(offset)},
            {"absolute_bar_offset", std::to_string(offset)},
            {"value", hex32(value)},
            {"value_dec", std::to_string(value)}
        }};
    }

    if (offset > ctx_.reg_size ||
        sizeof(value) > static_cast<size_t>(ctx_.reg_size - offset) ||
        !common::bar::read32(ctx_.device_ctx,
                             ctx_.bar_index,
                             ctx_.reg_base_offset + offset,
                             value)) {
        return {"pcie_bar_read32", ctx_.target_name, false, {
            {"bar_index", std::to_string(ctx_.bar_index)},
            {"offset", std::to_string(offset)},
            {"reg_size", std::to_string(ctx_.reg_size)}
        }, "BAR read32 failed", "check BAR mmap, offset alignment, and policy reg_size"};
    }

    return {"pcie_bar_read32", ctx_.target_name, true, {
        {"bar_index", std::to_string(ctx_.bar_index)},
        {"offset", std::to_string(offset)},
        {"absolute_bar_offset", std::to_string(ctx_.reg_base_offset + offset)},
        {"value", hex32(value)},
        {"value_dec", std::to_string(value)}
    }};
}

TestResult GenericPCIeImpl::bar_scan32(TestInfo& ti)
{
    constexpr uint64_t max_words = 256;

    auto offset = common::args::get_u64(ti.args, "offset", 0);
    auto words = common::args::get_u64(ti.args, "words", 16);
    auto requested_bar = ti.args.find("bar_index");

    if ((offset % sizeof(uint32_t)) != 0) {
        return {"pcie_bar_scan32", ctx_.target_name, false, {
            {"offset", std::to_string(offset)}
        }, "offset must be 4-byte aligned"};
    }
    if (words == 0 || words > max_words) {
        return {"pcie_bar_scan32", ctx_.target_name, false, {
            {"words", std::to_string(words)},
            {"max_words", std::to_string(max_words)}
        }, "words must be in range 1..256"};
    }

    std::vector<uint32_t> values(static_cast<size_t>(words), 0);
    auto bytes = words * sizeof(uint32_t);
    if (requested_bar != ti.args.end()) {
        auto bar_index = static_cast<uint32_t>(
            common::args::get_u64(ti.args, "bar_index", ctx_.bar_index));
        if (!common::bar::read(ctx_.device_ctx,
                               bar_index,
                               offset,
                               values.data(),
                               static_cast<size_t>(bytes))) {
            const auto* bar = common::bar::find(ctx_.device_ctx, bar_index);
            return {"pcie_bar_scan32", ctx_.target_name, false, {
                {"bar_index", std::to_string(bar_index)},
                {"offset", std::to_string(offset)},
                {"words", std::to_string(words)},
                {"bytes", std::to_string(bytes)},
                {"mapped_size", std::to_string(bar == nullptr ? 0 : bar->mapped_size)}
            }, "BAR scan32 failed", "check BAR mmap, offset range, and BAR size"};
        }

        TestMetrics metrics = {
            {"bar_index", std::to_string(bar_index)},
            {"offset", std::to_string(offset)},
            {"absolute_bar_offset", std::to_string(offset)},
            {"words", std::to_string(words)}
        };
        for (size_t i = 0; i < values.size(); ++i) {
            metrics["word_" + std::to_string(i)] = hex32(values[i]);
        }

        return {"pcie_bar_scan32", ctx_.target_name, true, metrics};
    }

    if (offset > ctx_.reg_size ||
        bytes > ctx_.reg_size - offset ||
        !common::bar::read(ctx_.device_ctx,
                           ctx_.bar_index,
                           ctx_.reg_base_offset + offset,
                           values.data(),
                           static_cast<size_t>(bytes))) {
        return {"pcie_bar_scan32", ctx_.target_name, false, {
            {"bar_index", std::to_string(ctx_.bar_index)},
            {"offset", std::to_string(offset)},
            {"words", std::to_string(words)},
            {"bytes", std::to_string(bytes)},
            {"reg_size", std::to_string(ctx_.reg_size)}
        }, "BAR scan32 failed", "check BAR mmap, offset range, and policy reg_size"};
    }

    TestMetrics metrics = {
        {"bar_index", std::to_string(ctx_.bar_index)},
        {"offset", std::to_string(offset)},
        {"absolute_bar_offset", std::to_string(ctx_.reg_base_offset + offset)},
        {"words", std::to_string(words)}
    };
    for (size_t i = 0; i < values.size(); ++i) {
        metrics["word_" + std::to_string(i)] = hex32(values[i]);
    }

    return {"pcie_bar_scan32", ctx_.target_name, true, metrics};
}

DmaTransferResult GenericPCIeImpl::dma_copy_h2d(const DmaTransferRequest& req)
{
    (void)req;
    return {false, 0, 0, "UNIMPLEMENTED", "PCIe H2D DMA copy is not implemented for " + ctx_.target_name};
}

DmaTransferResult GenericPCIeImpl::dma_copy_d2h(const DmaTransferRequest& req)
{
    (void)req;
    return {false, 0, 0, "UNIMPLEMENTED", "PCIe D2H DMA copy is not implemented for " + ctx_.target_name};
}

// Generic PMU impl. Products without a PMU module, or without a specific PMU
// operation, naturally land here and report UNIMPLEMENTED.
GenericPMUImpl::GenericPMUImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestResult GenericPMUImpl::PmuIpcRequestStart(TestInfo& ti)
{
    return make_unimplemented_result(ti, "PMU IPC request start is not implemented for " + ctx_.target_name);
}

TestResult GenericPMUImpl::PmuIpcRequestExec(TestInfo& ti)
{
    return make_unimplemented_result(ti, "PMU IPC request exec is not implemented for " + ctx_.target_name);
}

TestResult GenericPMUImpl::PmuIpcRequestFinish(TestInfo& ti)
{
    return make_unimplemented_result(ti, "PMU IPC request finish is not implemented for " + ctx_.target_name);
}

TestResult GenericPMUImpl::PmuRegRead(TestInfo& ti)
{
    return make_unimplemented_result(ti, "PMU register read is not implemented for " + ctx_.target_name);
}

TestResult GenericPMUImpl::PmuRegWrite(TestInfo& ti)
{
    return make_unimplemented_result(ti, "PMU register write is not implemented for " + ctx_.target_name);
}

TestResult GenericPMUImpl::PmuRegCheck(TestInfo& ti)
{
    return make_unimplemented_result(ti, "PMU register check is not implemented for " + ctx_.target_name);
}

// Generic ISI impl. Product-specific ISI impls override link setup/status flows
// when their topology and register programming are known.
GenericISIImpl::GenericISIImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestResult GenericISIImpl::IsiLinkup(TestInfo& ti)
{
    return make_unimplemented_result(ti, "ISI linkup is not implemented for " + ctx_.target_name);
}

TestResult GenericISIImpl::IsiSetup(TestInfo& ti)
{
    return make_unimplemented_result(ti, "ISI setup is not implemented for " + ctx_.target_name);
}

// Generic DDP impl. DDP currently exposes only the dmem link-up verification
// test; product-specific impls override this flow when supported.
GenericDDPImpl::GenericDDPImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestResult GenericDDPImpl::DdpDmemLinkupVerify(TestInfo& ti)
{
    return make_unimplemented_result(ti, "DDP dmem linkup verify is not implemented for " + ctx_.target_name);
}

// Generic DMC impl. DMC children use this fallback unless the product exposes and
// implements memory-controller diagnostics.
GenericDMCImpl::GenericDMCImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestResult GenericDMCImpl::DmcStatusCheck(TestInfo& ti)
{
    return make_unimplemented_result(ti, "DMC status check is not implemented for " + ctx_.target_name);
}

TestResult GenericDMCImpl::DmcRegScan(TestInfo& ti)
{
    return make_unimplemented_result(ti, "DMC register scan is not implemented for " + ctx_.target_name);
}

// Generic impl builders used by Implementer as the final fallback path.
std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<GenericTPUImpl>(ctx);
}

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<GenericPCIeImpl>(ctx);
}

std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<GenericPMUImpl>(ctx);
}

std::unique_ptr<ISIImpl> make_isi_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<GenericISIImpl>(ctx);
}

std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<GenericDDPImpl>(ctx);
}

std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<GenericDMCImpl>(ctx);
}

}
