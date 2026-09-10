#include "generic/generic_impl.h"

#include <utility>

namespace atlas_impl {
namespace {

class AtlasTPUImpl : public generic_impl::GenericTPUImpl {
public:
    explicit AtlasTPUImpl(ModuleImplContext ctx)
        : generic_impl::GenericTPUImpl(std::move(ctx))
    {
    }

    TestStatus Identify(TestInfo& ti) override
    {
        if (ti.logger != nullptr) {
            ti.logger->info("TPU identify product=ATLAS bdf=" + ctx_.device_ctx.bdf +
                            " vendor_id=" + std::to_string(ctx_.device_ctx.vendor_id) +
                            " device_id=" + std::to_string(ctx_.device_ctx.device_id) +
                            " chip_id=ATLAS_CHIP_PSEUDO revision=A0");
        }
        return TestStatus::OK;
    }
};

}

std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasTPUImpl>(ctx);
}

}
