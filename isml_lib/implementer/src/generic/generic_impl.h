#pragma once

#include "diag/implementer/Implementer.h"

#include <memory>

namespace generic_impl {

class GenericTPUImpl : public TPUImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericTPUImpl(ModuleImplContext ctx);

    TestStatus identify(TestInfo& ti) override;
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

    TestStatus ipc_request_start(TestInfo& ti) override;
    TestStatus ipc_request_exec(TestInfo& ti) override;
    TestStatus ipc_request_finish(TestInfo& ti) override;
    TestStatus reg_read(TestInfo& ti) override;
    TestStatus reg_write(TestInfo& ti) override;
    TestStatus reg_check(TestInfo& ti) override;
};

class GenericISIImpl : public ISIImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericISIImpl(ModuleImplContext ctx);

    TestStatus linkup(TestInfo& ti) override;
    TestStatus setup(TestInfo& ti) override;
};

class GenericDDPImpl : public DDPImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericDDPImpl(ModuleImplContext ctx);

    TestStatus dmem_linkup_verify(TestInfo& ti) override;
};

class GenericDMCImpl : public DMCImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericDMCImpl(ModuleImplContext ctx);

    TestStatus status_check(TestInfo& ti) override;
    TestStatus reg_scan(TestInfo& ti) override;
};

std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx);
std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx);
std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx);
std::unique_ptr<ISIImpl> make_isi_impl(const ModuleImplContext& ctx);
std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx);
std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx);

}
