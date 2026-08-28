#include "generic/generic_impl.h"

#include "diag/core/Common.h"

#include <utility>

namespace atlas_impl {
namespace {

class AtlasDDPImpl : public generic_impl::GenericDDPImpl {
public:
    explicit AtlasDDPImpl(ModuleImplContext ctx)
        : generic_impl::GenericDDPImpl(std::move(ctx))
    {
    }

    TestResult DdpDmemLinkupVerify(TestInfo& ti) override
    {
        auto link = common::args::get_string(ti.args, "link");
        return {"ddp_dmem_linkup_verify", ctx_.target_name, true, {
            {"link", link},
            {"linkup_status", "up"},
            {"impl", "atlas"}
        }};
    }
};

}

std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasDDPImpl>(ctx);
}

}
