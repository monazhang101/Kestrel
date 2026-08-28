#include "generic/generic_impl.h"

#include <utility>

namespace atlas_m_impl {
namespace {

class AtlasMPMUImpl : public generic_impl::GenericPMUImpl {
public:
    explicit AtlasMPMUImpl(ModuleImplContext ctx)
        : generic_impl::GenericPMUImpl(std::move(ctx))
    {
    }
};

}

std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasMPMUImpl>(ctx);
}

}
