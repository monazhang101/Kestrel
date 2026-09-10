#include "diag/module/PMUModule.h"

// pmu_ipc_request_start : To start a PMU IPC transaction and reserve the command mailbox.
// @input: args["request_id"] resolved by YAML defaults.
// @output: TestStatus; IPC details are written to the testcase log.
TestStatus PMUModule::PmuIpcRequestStart(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->PmuIpcRequestStart(ti);
}

// pmu_ipc_request_exec : To execute a previously prepared PMU IPC request.
// @input: args["opcode"] PMU IPC command opcode.
// @output: TestStatus; IPC details are written to the testcase log.
TestStatus PMUModule::PmuIpcRequestExec(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->PmuIpcRequestExec(ti);
}

// pmu_ipc_request_finish : To finish a PMU IPC transaction and collect completion status.
// @input: args["timeout_ms"] resolved by YAML defaults.
// @output: TestStatus; IPC details are written to the testcase log.
TestStatus PMUModule::PmuIpcRequestFinish(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->PmuIpcRequestFinish(ti);
}
