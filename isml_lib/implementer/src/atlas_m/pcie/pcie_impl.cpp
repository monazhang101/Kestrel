#include "generic/generic_impl.h"


namespace atlas_m_impl {
namespace {

class AtlasMPCIeImpl : public generic_impl::GenericPCIeImpl {
public:
};

}

std::unique_ptr<PCIeImpl> make_pcie_impl()
{
    return std::make_unique<AtlasMPCIeImpl>();
}

}
