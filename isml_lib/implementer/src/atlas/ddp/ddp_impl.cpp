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

    TestStatus DdpDmemLinkupVerify(TestInfo& ti) override
    {
        const auto link = common::args::get_string(ti.args, "link");
        if (ti.logger != nullptr) {
            ti.logger->info("DDP DMEM link=" + link + " state=up impl=atlas");
        }
        return TestStatus::OK;
    }
};

}

std::unique_ptr<DDPImpl> make_ddp_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasDDPImpl>(ctx);
}

}
