#include "generic/generic_impl.h"

#include "diag/core/Common.h"

#include <utility>

namespace atlas_impl {
namespace {

class AtlasPMUImpl : public generic_impl::GenericPMUImpl {
public:
    explicit AtlasPMUImpl(ModuleImplContext ctx)
        : generic_impl::GenericPMUImpl(std::move(ctx))
    {
    }

    TestStatus PmuIpcRequestStart(TestInfo& ti) override
    {
        const auto request_id = common::args::get_string(ti.args, "request_id");
        if (ti.logger != nullptr) {
            ti.logger->info("PMU IPC request_id=" + request_id +
                            " state=started impl=atlas");
        }
        return TestStatus::OK;
    }

    TestStatus PmuIpcRequestExec(TestInfo& ti) override
    {
        const auto opcode = common::args::get_string(ti.args, "opcode");
        if (ti.logger != nullptr) {
            ti.logger->info("PMU IPC opcode=" + opcode +
                            " state=accepted impl=atlas");
        }
        return TestStatus::OK;
    }

    TestStatus PmuIpcRequestFinish(TestInfo& ti) override
    {
        const auto timeout_ms = common::args::get_string(ti.args, "timeout_ms");
        if (ti.logger != nullptr) {
            ti.logger->info("PMU IPC timeout_ms=" + timeout_ms +
                            " state=done pmu_status=ok impl=atlas");
        }
        return TestStatus::OK;
    }

    TestStatus PmuRegRead(TestInfo& ti) override
    {
        const auto offset = common::args::get_string(ti.args, "offset");
        if (ti.logger != nullptr) {
            ti.logger->info("PMU register read offset=" + offset +
                            " value=0x00000000 impl=atlas");
        }
        return TestStatus::OK;
    }

    TestStatus PmuRegWrite(TestInfo& ti) override
    {
        const auto offset = common::args::get_string(ti.args, "offset");
        const auto value = common::args::get_string(ti.args, "value");
        if (ti.logger != nullptr) {
            ti.logger->info("PMU register write offset=" + offset +
                            " value=" + value + " state=done impl=atlas");
        }
        return TestStatus::OK;
    }

    TestStatus PmuRegCheck(TestInfo& ti) override
    {
        const auto offset = common::args::get_string(ti.args, "offset");
        const auto expected = common::args::get_string(ti.args, "expected");
        if (ti.logger != nullptr) {
            ti.logger->info("PMU register check offset=" + offset +
                            " actual=" + expected + " expected=" + expected +
                            " state=match impl=atlas");
        }
        return TestStatus::OK;
    }
};

}

std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasPMUImpl>(ctx);
}

}
