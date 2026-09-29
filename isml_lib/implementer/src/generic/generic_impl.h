#pragma once

#include "diag/implementer/Implementer.h"

namespace generic_impl {

class GenericPCIeImpl : public PCIeImpl {
public:
    TestStatus dma_copy(TestInfo& ti, const DmaTransferRequest& req) override;
};

std::unique_ptr<PCIeImpl> make_pcie_impl();

}
