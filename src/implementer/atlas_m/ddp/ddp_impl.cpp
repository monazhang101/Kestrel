#include "generic/generic_impl.h"

#include "diag/core/Common.h"

#include <utility>

namespace atlas_m_impl {
namespace {

class AtlasMDDPImpl : public generic_impl::GenericDDPImpl {
public:
    explicit AtlasMDDPImpl(ModuleImplContext ctx)
        : generic_impl::GenericDDPImpl(std::move(ctx))
    {
    }

    TestResult DdpDmemLinkupVerify(TestInfo& ti) override
    {
        auto link = common::args::get_string(ti.args, "link");
        return {"ddp_dmem_linkup_verify", ctx_.target_name, true, {
            {"link", link},
            {"linkup_status", "up"},
            {"impl", "atlas_m"}
        }};
    }
};

}

std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasMDDPImpl>(ctx);
}

}
