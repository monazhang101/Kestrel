#include "generic/generic_impl.h"

#include "diag/core/Common.h"

#include <utility>

namespace atlas_m_impl {
namespace {

class AtlasMISIImpl : public generic_impl::GenericISIImpl {
public:
    explicit AtlasMISIImpl(ModuleImplContext ctx)
        : generic_impl::GenericISIImpl(std::move(ctx))
    {
    }

    TestResult IsiLinkup(TestInfo& ti) override
    {
        (void)ti.args;
        return {"isi_linkup", ctx_.target_name, true, {
            {"link_id", std::to_string(ctx_.index)},
            {"link_status", "up"},
            {"lane_ready_bitmap", "0x0f"},
            {"error_count", "0"},
            {"impl", "atlas_m"}
        }};
    }

    TestResult IsiSetup(TestInfo& ti) override
    {
        auto mode = common::args::get_string(ti.args, "mode");
        return {"isi_setup", ctx_.target_name, true, {
            {"link_id", std::to_string(ctx_.index)},
            {"mode", mode},
            {"setup_status", "done"},
            {"impl", "atlas_m"}
        }};
    }
};

}

std::unique_ptr<ISIImpl> make_isi_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasMISIImpl>(ctx);
}

}
