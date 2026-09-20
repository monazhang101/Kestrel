#pragma once

#include "diag/implementer/Implementer.h"

namespace generic_impl {

class GenericPCIeImpl : public PCIeImpl {
protected:
    ModuleImplContext ctx_;

public:
    explicit GenericPCIeImpl(ModuleImplContext ctx);

    TestStatus bar_read32(TestInfo& ti) override;
    TestStatus bar_scan32(TestInfo& ti) override;
    TestStatus dma_copy(TestInfo& ti, const DmaTransferRequest& req) override;
};

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx);

}
