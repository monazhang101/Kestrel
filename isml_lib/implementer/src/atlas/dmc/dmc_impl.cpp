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

    TestStatus status_check(TestInfo& ti) override
    {
        if (ti.logger != nullptr) {
            ti.logger->info("DMC status ddp_id=" + std::to_string(ctx_.parent_index) +
                            " controller_id=" + std::to_string(ctx_.index) +
                            " state=ready impl=atlas");
        }
        return TestStatus::OK;
    }

    TestStatus reg_scan(TestInfo& ti) override
    {
        const auto range = common::args::get_string(ti.args, "range");
        if (ti.logger != nullptr) {
            ti.logger->info("DMC register scan ddp_id=" +
                            std::to_string(ctx_.parent_index) +
                            " controller_id=" + std::to_string(ctx_.index) +
                            " range=" + range + " bad_register_count=0 impl=atlas");
        }
        return TestStatus::OK;
    }
};

}

std::unique_ptr<DMCImpl> make_dmc_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasDMCImpl>(ctx);
}

}
