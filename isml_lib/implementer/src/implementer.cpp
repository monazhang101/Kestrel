#include "diag/implementer/Implementer.h"

#include "generic/generic_impl.h"

namespace atlas_impl {

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx);

}

namespace atlas_m_impl {

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx);

}

Implementer::Implementer(TPUType tpu_type)
    : tpu_type_(tpu_type)
{
}

std::unique_ptr<PCIeImpl> Implementer::pcie_impl(const ModuleImplContext& ctx) const
{
    if (tpu_type_ == TPUType::Atlas) {
        return atlas_impl::make_pcie_impl(ctx);
    }
    if (tpu_type_ == TPUType::AtlasM) {
        return atlas_m_impl::make_pcie_impl(ctx);
    }
    return generic_impl::make_pcie_impl(ctx);
}
