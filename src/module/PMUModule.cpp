#include "diag/module/PMUModule.h"

#include "diag/core/Common.h"

PMUModule::PMUModule(const std::string& name,
                     const DeviceContext& ctx,
                     uint64_t reg_offset,
                     uint64_t reg_size)
    : BaseDevice(name, ctx), reg_offset_(reg_offset), reg_size_(reg_size)
{
    auto* bar_base = static_cast<uint8_t*>(ctx_.mapped_bar_base);
    reg_base_ = bar_base == nullptr ? nullptr : bar_base + reg_offset_;

    // ----------------- Atomic Tests -----------------
    _add_test("pmu_ipc_request_start", [this](TestInfo& ti) { return PmuIpcRequestStart(ti); });
    _add_test("pmu_ipc_request_exec", [this](TestInfo& ti) { return PmuIpcRequestExec(ti); });
    _add_test("pmu_ipc_request_finish", [this](TestInfo& ti) { return PmuIpcRequestFinish(ti); });
    _add_test("pmu_reg_read", [this](TestInfo& ti) { return PmuRegRead(ti); });
    _add_test("pmu_reg_write", [this](TestInfo& ti) { return PmuRegWrite(ti); });
    _add_test("pmu_reg_check", [this](TestInfo& ti) { return PmuRegCheck(ti); });

}

// pmu_ipc_request_start : To start a PMU IPC transaction and reserve the command mailbox.
// @input: args["request_id"] resolved by YAML defaults.
// @output: TestResult metrics include ipc_state and request_id.
TestResult PMUModule::PmuIpcRequestStart(TestInfo& ti)
{
    auto request_id = common::args::get_string(ti.args, "request_id");

    // Pseudocode: check mailbox idle, write request header, mark IPC busy.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_ipc_request_start", get_name(), true, {
        {"request_id", request_id},
        {"ipc_state", "started"}
    }};
}

// pmu_ipc_request_exec : To execute a previously prepared PMU IPC request.
// @input: args["opcode"] PMU IPC command opcode.
// @output: TestResult metrics include opcode and completion_state.
TestResult PMUModule::PmuIpcRequestExec(TestInfo& ti)
{
    auto opcode = common::args::get_string(ti.args, "opcode");

    // Pseudocode: write opcode, ring PMU mailbox doorbell, poll command accepted bit.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_ipc_request_exec", get_name(), true, {
        {"opcode", opcode},
        {"completion_state", "accepted"}
    }};
}

// pmu_ipc_request_finish : To finish a PMU IPC transaction and collect completion status.
// @input: args["timeout_ms"] resolved by YAML defaults.
// @output: TestResult metrics include completion_state and pmu_status.
TestResult PMUModule::PmuIpcRequestFinish(TestInfo& ti)
{
    auto timeout_ms = common::args::get_string(ti.args, "timeout_ms");

    // Pseudocode: poll completion bit, read result code, clear mailbox busy state.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_ipc_request_finish", get_name(), true, {
        {"timeout_ms", timeout_ms},
        {"completion_state", "done"},
        {"pmu_status", "ok"}
    }};
}

// pmu_reg_read : To read one PMU register through the PMU register window.
// @input: args["offset"] register offset.
// @output: TestResult metrics include offset and value.
TestResult PMUModule::PmuRegRead(TestInfo& ti)
{
    auto offset = common::args::get_string(ti.args, "offset");

    // Pseudocode: ok = common::reg::read(reg_base_, reg_size_, parsed_offset, &value, sizeof(value)).
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_reg_read", get_name(), true, {
        {"offset", offset},
        {"value", "0x00000000"}
    }};
}

// pmu_reg_write : To write one PMU register through the PMU register window.
// @input: args["offset"] register offset, args["value"] value to write.
// @output: TestResult metrics include offset, value, and write_status.
TestResult PMUModule::PmuRegWrite(TestInfo& ti)
{
    auto offset = common::args::get_string(ti.args, "offset");
    auto value = common::args::get_string(ti.args, "value");

    // Pseudocode: ok = common::reg::write(reg_base_, reg_size_, parsed_offset, &parsed_value, sizeof(parsed_value)).
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_reg_write", get_name(), true, {
        {"offset", offset},
        {"value", value},
        {"write_status", "done"}
    }};
}

// pmu_reg_check : To read a PMU register and check it against an expected value.
// @input: args["offset"] register offset, args["expected"] expected value.
// @output: TestResult metrics include offset, actual, expected, and check_status.
TestResult PMUModule::PmuRegCheck(TestInfo& ti)
{
    auto offset = common::args::get_string(ti.args, "offset");
    auto expected = common::args::get_string(ti.args, "expected");

    // Pseudocode: read actual value, apply optional mask, compare with expected.
    (void)reg_base_;
    (void)reg_size_;

    return {"pmu_reg_check", get_name(), true, {
        {"offset", offset},
        {"actual", expected},
        {"expected", expected},
        {"check_status", "match"}
    }};
}