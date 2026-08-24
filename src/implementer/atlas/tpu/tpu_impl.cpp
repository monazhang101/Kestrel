#include "generic/generic_impl.h"

#include "diag/core/Common.h"

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

    TestResult SocGpioDirSet(TestInfo& ti) override
    {
        auto pin = common::args::get_string(ti.args, "pin");
        auto direction = common::args::get_string(ti.args, "direction");
        return {"soc_gpio_dir_set", ctx_.target_name, true, {
            {"pin", pin},
            {"direction", direction},
            {"status", "configured"},
            {"impl", "atlas"}
        }};
    }

    TestResult SocGpioRead(TestInfo& ti) override
    {
        auto pin = common::args::get_string(ti.args, "pin");
        return {"soc_gpio_read", ctx_.target_name, true, {
            {"pin", pin},
            {"value", "1"},
            {"impl", "atlas"}
        }};
    }

    TestResult SocGpioWrite(TestInfo& ti) override
    {
        auto pin = common::args::get_string(ti.args, "pin");
        auto value = common::args::get_string(ti.args, "value");
        return {"soc_gpio_write", ctx_.target_name, true, {
            {"pin", pin},
            {"value", value},
            {"status", "written"},
            {"impl", "atlas"}
        }};
    }
};

}

std::unique_ptr<TPUImpl> make_tpu_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasTPUImpl>(ctx);
}

}
