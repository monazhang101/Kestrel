#include "generic/generic_impl.h"

#include "diag/core/Common.h"

#include <utility>

namespace atlas_impl {
namespace {

class AtlasDMCImpl : public generic_impl::GenericDMCImpl {
public:
    explicit AtlasDMCImpl(ModuleImplContext ctx)
        : generic_impl::GenericDMCImpl(std::move(ctx))
    {
    }

    TestResult DmcStatusCheck(TestInfo& ti) override
    {
        (void)ti.args;
        return {"dmc_status_check", ctx_.target_name, true, {
            {"ddp_id", std::to_string(ctx_.parent_index)},
            {"controller_id", std::to_string(ctx_.index)},
            {"status", "ready"},
            {"impl", "atlas"}
        }};
    }

    TestResult DmcRegScan(TestInfo& ti) override
    {
        auto range = common::args::get_string(ti.args, "range");
        return {"dmc_reg_scan", ctx_.target_name, true, {
            {"ddp_id", std::to_string(ctx_.parent_index)},
            {"controller_id", std::to_string(ctx_.index)},
            {"scanned_range", range},
            {"bad_register_count", "0"},
            {"impl", "atlas"}
        }};
    }
};

}

std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasDMCImpl>(ctx);
}

}
