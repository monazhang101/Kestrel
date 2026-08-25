#include "generic_impl.h"

#include <utility>

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
