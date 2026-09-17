#pragma once

#include "diag/implementer/Implementer.h"

#include <memory>

namespace generic_impl {

class GenericTPUImpl : public TPUImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericTPUImpl(ModuleImplContext ctx);

    TestStatus Identify(TestInfo& ti) override;
};

class GenericPCIeImpl : public PCIeImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericPCIeImpl(ModuleImplContext ctx);

    TestStatus bar_read32(TestInfo& ti) override;
    TestStatus bar_scan32(TestInfo& ti) override;
    TestStatus dma_copy(TestInfo& ti, const DmaTransferRequest& req) override;
};

class GenericPMUImpl : public PMUImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericPMUImpl(ModuleImplContext ctx);

    TestStatus PmuIpcRequestStart(TestInfo& ti) override;
    TestStatus PmuIpcRequestExec(TestInfo& ti) override;
    TestStatus PmuIpcRequestFinish(TestInfo& ti) override;
    TestStatus PmuRegRead(TestInfo& ti) override;
    TestStatus PmuRegWrite(TestInfo& ti) override;
    TestStatus PmuRegCheck(TestInfo& ti) override;
};

class GenericISIImpl : public ISIImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericISIImpl(ModuleImplContext ctx);

    TestStatus IsiLinkup(TestInfo& ti) override;
    TestStatus IsiSetup(TestInfo& ti) override;
};

class GenericDDPImpl : public DDPImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericDDPImpl(ModuleImplContext ctx);

    TestStatus DdpDmemLinkupVerify(TestInfo& ti) override;
};

class GenericDMCImpl : public DMCImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericDMCImpl(ModuleImplContext ctx);

    TestStatus DmcStatusCheck(TestInfo& ti) override;
    TestStatus DmcRegScan(TestInfo& ti) override;
};

std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx);
std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx);
std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx);
std::unique_ptr<ISIImpl> make_isi_impl(const ModuleImplContext& ctx);
std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx);
std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx);

}
