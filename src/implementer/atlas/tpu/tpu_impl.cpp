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

    TestResult Identify(TestInfo& ti) override
    {
        (void)ti.args;
        return {"identify", ctx_.target_name, true, {
            {"product", "ATLAS"},
            {"bdf", ctx_.device_ctx.bdf},
            {"vendor_id", "0x" + std::to_string(ctx_.device_ctx.vendor_id)},
            {"device_id", "0x" + std::to_string(ctx_.device_ctx.device_id)},
            {"chip_id", "ATLAS_CHIP_PSEUDO"},
            {"revision", "A0"}
        }};
    }
};

}

std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasTPUImpl>(ctx);
}

}
