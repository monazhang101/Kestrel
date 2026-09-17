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

    TestStatus linkup(TestInfo& ti) override
    {
        if (ti.logger != nullptr) {
            ti.logger->info("ISI link status link_id=" + std::to_string(ctx_.index) +
                            " state=up lane_ready_bitmap=0x0f error_count=0 impl=atlas_m");
        }
        return TestStatus::OK;
    }

    TestStatus setup(TestInfo& ti) override
    {
        const auto mode = common::args::get_string(ti.args, "mode");
        if (ti.logger != nullptr) {
            ti.logger->info("ISI setup link_id=" + std::to_string(ctx_.index) +
                            " mode=" + mode + " state=done impl=atlas_m");
        }
        return TestStatus::OK;
    }
};

}

std::unique_ptr<ISIImpl> make_isi_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasMISIImpl>(ctx);
}

}
