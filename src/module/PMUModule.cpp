#include "diag/module/PMUModule.h"

#include <utility>

PMUModule::PMUModule(const std::string& name,
                     const DeviceContext& ctx,
                     const ModuleInstanceConfig& config,
                     std::unique_ptr<PMUImpl> impl)
    : BaseDevice(name, ctx),
      reg_offset_(config.reg_offset),
      reg_size_(config.reg_size),
      impl_(std::move(impl))
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
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PMU implementation is not bound");
    }
    return impl_->PmuIpcRequestStart(ti);
}

// pmu_ipc_request_exec : To execute a previously prepared PMU IPC request.
// @input: args["opcode"] PMU IPC command opcode.
// @output: TestResult metrics include opcode and completion_state.
TestResult PMUModule::PmuIpcRequestExec(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PMU implementation is not bound");
    }
    return impl_->PmuIpcRequestExec(ti);
}

// pmu_ipc_request_finish : To finish a PMU IPC transaction and collect completion status.
// @input: args["timeout_ms"] resolved by YAML defaults.
// @output: TestResult metrics include completion_state and pmu_status.
TestResult PMUModule::PmuIpcRequestFinish(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PMU implementation is not bound");
    }
    return impl_->PmuIpcRequestFinish(ti);
}

// pmu_reg_read : To read one PMU register through the PMU register window.
// @input: args["offset"] register offset.
// @output: TestResult metrics include offset and value.
TestResult PMUModule::PmuRegRead(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PMU implementation is not bound");
    }
    return impl_->PmuRegRead(ti);
}

// pmu_reg_write : To write one PMU register through the PMU register window.
// @input: args["offset"] register offset, args["value"] value to write.
// @output: TestResult metrics include offset, value, and write_status.
TestResult PMUModule::PmuRegWrite(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PMU implementation is not bound");
    }
    return impl_->PmuRegWrite(ti);
}

// pmu_reg_check : To read a PMU register and check it against an expected value.
// @input: args["offset"] register offset, args["expected"] expected value.
// @output: TestResult metrics include offset, actual, expected, and check_status.
TestResult PMUModule::PmuRegCheck(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PMU implementation is not bound");
    }
    return impl_->PmuRegCheck(ti);
}
