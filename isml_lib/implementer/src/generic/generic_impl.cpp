#include "generic_impl.h"

#include <memory>

namespace generic_impl {

TestStatus GenericPCIeImpl::dma_copy(TestInfo& ti,
                                         const DmaTransferRequest& req)
{
    (void)req;
    return make_unimplemented_status(
        ti, "PCIe DMA copy is not implemented for " + ti.target_name);
}

std::unique_ptr<PCIeImpl> make_pcie_impl()
{
    return std::make_unique<GenericPCIeImpl>();
}

}
