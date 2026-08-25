#include "diag/module/PMUModule.h"

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
