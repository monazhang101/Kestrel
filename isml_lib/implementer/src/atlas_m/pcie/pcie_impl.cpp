#include "generic/generic_impl.h"

#include <utility>

namespace atlas_m_impl {
namespace {

class AtlasMPCIeImpl : public generic_impl::GenericPCIeImpl {
public:
    explicit AtlasMPCIeImpl(ModuleImplContext ctx)
        : generic_impl::GenericPCIeImpl(std::move(ctx))
    {
    }

    LinkStatus link_status_get() override
    {
        LinkStatus status;
        status.ok = true;
        status.current_speed = "gen4";
        status.current_width = "x16";
        status.metrics = {{"impl", "atlas_m"}};
        return status;
    }

};

}

std::unique_ptr<PCIeImpl> make_pcie_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasMPCIeImpl>(ctx);
}

}
