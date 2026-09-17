#include "generic_impl.h"

#include "diag/core/Common.h"

#include <cstdint>
#include <iomanip>
#include <limits>
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

std::string hex64(uint64_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
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

TestStatus GenericTPUImpl::Identify(TestInfo& ti)
{
    return make_unimplemented_status(
        ti, "TPU identify is not implemented for " + ctx_.target_name);
}

// Generic PCIe impl. This is the fallback for BAR and DMA operations when
// a product-specific PCIe implementation does not provide an override.
GenericPCIeImpl::GenericPCIeImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestStatus GenericPCIeImpl::bar_read32(TestInfo& ti)
{
    const auto offset = common::args::get_u64(ti.args, "offset");

    if ((offset % sizeof(uint32_t)) != 0) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR read offset must be 4-byte aligned: offset=" +
                             hex64(offset));
        }
        return TestStatus::INVALID;
    }

    uint32_t value = 0;
    const uint32_t bar_index = ctx_.bar_index;
    if (offset > ctx_.reg_size ||
        sizeof(value) > static_cast<size_t>(ctx_.reg_size - offset)) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "BAR read is outside module range\n"
                "       module_offset       = " + hex64(offset) +
                "\n       module_size         = " + hex64(ctx_.reg_size));
        }
        return TestStatus::INVALID;
    }

    if (ctx_.reg_base_offset >
        std::numeric_limits<uint64_t>::max() - offset) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR read module offset overflow");
        }
        return TestStatus::INVALID;
    }
    const uint64_t absolute_offset = ctx_.reg_base_offset + offset;
    if (!common::bar::read32(ctx_.device_ctx,
                             bar_index,
                             absolute_offset,
                             value)) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "BAR read failed\n"
                "       bar_index           = " + std::to_string(bar_index) +
                "\n       module_offset       = " + hex64(offset) +
                "\n       absolute_bar_offset = " + hex64(absolute_offset));
        }
        return TestStatus::ERROR;
    }

    if (ti.logger != nullptr) {
        ti.logger->info(
            "BAR read completed\n"
            "       bar_index           = " + std::to_string(bar_index) +
            "\n       module_offset       = " + hex64(offset) +
            " (" + std::to_string(offset) + ")" +
            "\n       absolute_bar_offset = " + hex64(absolute_offset) +
            " (" + std::to_string(absolute_offset) + ")" +
            "\n       value               = " + hex32(value) +
            " (" + std::to_string(value) + ")");
    }
    return TestStatus::OK;
}

TestStatus GenericPCIeImpl::bar_scan32(TestInfo& ti)
{
    constexpr uint64_t MAX_WORDS = 256;

    const auto offset = common::args::get_u64(ti.args, "offset");
    const auto words = common::args::get_u64(ti.args, "words");

    if ((offset % sizeof(uint32_t)) != 0) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR scan offset must be 4-byte aligned: offset=" +
                             hex64(offset));
        }
        return TestStatus::INVALID;
    }
    if (words == 0 || words > MAX_WORDS) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR scan words must be in range 1.." +
                             std::to_string(MAX_WORDS));
        }
        return TestStatus::INVALID;
    }

    std::vector<uint32_t> values(static_cast<size_t>(words), 0);
    const auto bytes = words * sizeof(uint32_t);
    const uint32_t bar_index = ctx_.bar_index;
    if (offset > ctx_.reg_size || bytes > ctx_.reg_size - offset) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "BAR scan is outside module range\n"
                "       module_offset       = " + hex64(offset) +
                "\n       size_bytes          = " + std::to_string(bytes) +
                "\n       module_size         = " + hex64(ctx_.reg_size));
        }
        return TestStatus::INVALID;
    }

    if (ctx_.reg_base_offset >
        std::numeric_limits<uint64_t>::max() - offset) {
        if (ti.logger != nullptr) {
            ti.logger->error("BAR scan module offset overflow");
        }
        return TestStatus::INVALID;
    }
    const uint64_t absolute_offset = ctx_.reg_base_offset + offset;
    if (!common::bar::read(ctx_.device_ctx,
                           bar_index,
                           absolute_offset,
                           values.data(),
                           static_cast<size_t>(bytes))) {
        if (ti.logger != nullptr) {
            ti.logger->error(
                "BAR scan failed\n"
                "       bar_index           = " + std::to_string(bar_index) +
                "\n       module_offset       = " + hex64(offset) +
                "\n       absolute_bar_offset = " + hex64(absolute_offset) +
                "\n       size_bytes          = " + std::to_string(bytes));
        }
        return TestStatus::ERROR;
    }

    if (ti.logger != nullptr) {
        ti.logger->info(
            "BAR scan completed\n"
            "       bar_index           = " + std::to_string(bar_index) +
            "\n       module_offset       = " + hex64(offset) +
            " (" + std::to_string(offset) + ")" +
            "\n       absolute_bar_offset = " + hex64(absolute_offset) +
            " (" + std::to_string(absolute_offset) + ")" +
            "\n       word_count          = " + std::to_string(words));
        for (size_t i = 0; i < values.size(); ++i) {
            ti.logger->debug(
                "BAR scan word[" + std::to_string(i) + "]" +
                " address=" + hex64(absolute_offset + i * sizeof(uint32_t)) +
                " value=" + hex32(values[i]));
        }
    }
    return TestStatus::OK;
}

TestStatus GenericPCIeImpl::dma_copy(TestInfo& ti,
                                         const DmaTransferRequest& req)
{
    (void)req;
    return make_unimplemented_status(
        ti, "PCIe DMA copy is not implemented for " + ctx_.target_name);
}

// Generic PMU impl. Products without a PMU module, or without a specific PMU
// operation, naturally land here and report UNIMPLEMENTED.
GenericPMUImpl::GenericPMUImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestStatus GenericPMUImpl::PmuIpcRequestStart(TestInfo& ti)
{
    return make_unimplemented_status(ti, "PMU IPC request start is not implemented for " + ctx_.target_name);
}

TestStatus GenericPMUImpl::PmuIpcRequestExec(TestInfo& ti)
{
    return make_unimplemented_status(ti, "PMU IPC request exec is not implemented for " + ctx_.target_name);
}

TestStatus GenericPMUImpl::PmuIpcRequestFinish(TestInfo& ti)
{
    return make_unimplemented_status(ti, "PMU IPC request finish is not implemented for " + ctx_.target_name);
}

TestStatus GenericPMUImpl::PmuRegRead(TestInfo& ti)
{
    return make_unimplemented_status(ti, "PMU register read is not implemented for " + ctx_.target_name);
}

TestStatus GenericPMUImpl::PmuRegWrite(TestInfo& ti)
{
    return make_unimplemented_status(ti, "PMU register write is not implemented for " + ctx_.target_name);
}

TestStatus GenericPMUImpl::PmuRegCheck(TestInfo& ti)
{
    return make_unimplemented_status(ti, "PMU register check is not implemented for " + ctx_.target_name);
}

// Generic ISI impl. Product-specific ISI impls override link setup/status flows
// when their topology and register programming are known.
GenericISIImpl::GenericISIImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestStatus GenericISIImpl::IsiLinkup(TestInfo& ti)
{
    return make_unimplemented_status(ti, "ISI linkup is not implemented for " + ctx_.target_name);
}

TestStatus GenericISIImpl::IsiSetup(TestInfo& ti)
{
    return make_unimplemented_status(ti, "ISI setup is not implemented for " + ctx_.target_name);
}

// Generic DDP impl. DDP currently exposes only the dmem link-up verification
// test; product-specific impls override this flow when supported.
GenericDDPImpl::GenericDDPImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestStatus GenericDDPImpl::DdpDmemLinkupVerify(TestInfo& ti)
{
    return make_unimplemented_status(ti, "DDP dmem linkup verify is not implemented for " + ctx_.target_name);
}

// Generic DMC impl. DMC children use this fallback unless the product exposes and
// implements memory-controller diagnostics.
GenericDMCImpl::GenericDMCImpl(ModuleImplContext ctx)
    : ctx_(std::move(ctx))
{
}

TestStatus GenericDMCImpl::DmcStatusCheck(TestInfo& ti)
{
    return make_unimplemented_status(ti, "DMC status check is not implemented for " + ctx_.target_name);
}

TestStatus GenericDMCImpl::DmcRegScan(TestInfo& ti)
{
    return make_unimplemented_status(ti, "DMC register scan is not implemented for " + ctx_.target_name);
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
