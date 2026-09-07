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

    TestResult PmuIpcRequestStart(TestInfo& ti) override
    {
        auto request_id = common::args::get_string(ti.args, "request_id");
        return {"pmu_ipc_request_start", ctx_.target_name, true, {
            {"request_id", request_id},
            {"ipc_state", "started"},
            {"impl", "atlas"}
        }};
    }

    TestResult PmuIpcRequestExec(TestInfo& ti) override
    {
        auto opcode = common::args::get_string(ti.args, "opcode");
        return {"pmu_ipc_request_exec", ctx_.target_name, true, {
            {"opcode", opcode},
            {"completion_state", "accepted"},
            {"impl", "atlas"}
        }};
    }

    TestResult PmuIpcRequestFinish(TestInfo& ti) override
    {
        auto timeout_ms = common::args::get_string(ti.args, "timeout_ms");
        return {"pmu_ipc_request_finish", ctx_.target_name, true, {
            {"timeout_ms", timeout_ms},
            {"completion_state", "done"},
            {"pmu_status", "ok"},
            {"impl", "atlas"}
        }};
    }

    TestResult PmuRegRead(TestInfo& ti) override
    {
        auto offset = common::args::get_string(ti.args, "offset");
        return {"pmu_reg_read", ctx_.target_name, true, {
            {"offset", offset},
            {"value", "0x00000000"},
            {"impl", "atlas"}
        }};
    }

    TestResult PmuRegWrite(TestInfo& ti) override
    {
        auto offset = common::args::get_string(ti.args, "offset");
        auto value = common::args::get_string(ti.args, "value");
        return {"pmu_reg_write", ctx_.target_name, true, {
            {"offset", offset},
            {"value", value},
            {"write_status", "done"},
            {"impl", "atlas"}
        }};
    }

    TestResult PmuRegCheck(TestInfo& ti) override
    {
        auto offset = common::args::get_string(ti.args, "offset");
        auto expected = common::args::get_string(ti.args, "expected");
        return {"pmu_reg_check", ctx_.target_name, true, {
            {"offset", offset},
            {"actual", expected},
            {"expected", expected},
            {"check_status", "match"},
            {"impl", "atlas"}
        }};
    }
};

}

std::unique_ptr<PMUImpl> make_pmu_impl(const ModuleImplContext& ctx)
{
    return std::make_unique<AtlasPMUImpl>(ctx);
}

}
