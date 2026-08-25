#pragma once

#include "diag/implementer/Implementer.h"

#include <memory>

namespace generic_impl {

class GenericTPUImpl : public TPUImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericTPUImpl(ModuleImplContext ctx);

    TestResult Identify(TestInfo& ti) override;
};

class GenericPCIeImpl : public PCIeImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericPCIeImpl(ModuleImplContext ctx);

    LinkStatus link_status_get() override;
    DmaTransferResult dma_copy_h2d(const DmaTransferRequest& req) override;
    DmaTransferResult dma_copy_d2h(const DmaTransferRequest& req) override;
};

class GenericPMUImpl : public PMUImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericPMUImpl(ModuleImplContext ctx);

    TestResult PmuIpcRequestStart(TestInfo& ti) override;
    TestResult PmuIpcRequestExec(TestInfo& ti) override;
    TestResult PmuIpcRequestFinish(TestInfo& ti) override;
    TestResult PmuRegRead(TestInfo& ti) override;
    TestResult PmuRegWrite(TestInfo& ti) override;
    TestResult PmuRegCheck(TestInfo& ti) override;
};

class GenericISIImpl : public ISIImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericISIImpl(ModuleImplContext ctx);

    TestResult IsiLinkup(TestInfo& ti) override;
    TestResult IsiSetup(TestInfo& ti) override;
};

class GenericDDPImpl : public DDPImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericDDPImpl(ModuleImplContext ctx);

    TestResult DdpDmemLinkupVerify(TestInfo& ti) override;
};

class GenericDMCImpl : public DMCImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericDMCImpl(ModuleImplContext ctx);

    TestResult DmcStatusCheck(TestInfo& ti) override;
    TestResult DmcRegScan(TestInfo& ti) override;
};

std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx);
std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx);
std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx);
std::unique_ptr<ISIImpl> make_isi_impl(const ModuleImplContext& ctx);
std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx);
std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx);

}
