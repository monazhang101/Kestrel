#include "generic/generic_impl.h"

#include <utility>

namespace atlas_m_impl {
namespace {

class AtlasMDMCImpl : public generic_impl::GenericDMCImpl {
public:
    explicit AtlasMDMCImpl(ModuleImplContext ctx)
        : generic_impl::GenericDMCImpl(std::move(ctx))
    {
    }
};

}

std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasMDMCImpl>(ctx);
}

}
