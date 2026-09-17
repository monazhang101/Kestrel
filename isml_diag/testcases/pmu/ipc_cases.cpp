#include "diag/modules/PMUModule.h"

// ipc_request_start: Start a PMU IPC transaction and reserve the command mailbox.
// @input: args["request_id"] resolved by YAML defaults.
// @output: TestStatus; IPC details are written to the testcase log.
TestStatus PMUModule::ipc_request_start(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->ipc_request_start(ti);
}

// ipc_request_exec: Execute a previously prepared PMU IPC request.
// @input: args["opcode"] PMU IPC command opcode.
// @output: TestStatus; IPC details are written to the testcase log.
TestStatus PMUModule::ipc_request_exec(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->ipc_request_exec(ti);
}

// ipc_request_finish: Finish a PMU IPC transaction and collect completion status.
// @input: args["timeout_ms"] resolved by YAML defaults.
// @output: TestStatus; IPC details are written to the testcase log.
TestStatus PMUModule::ipc_request_finish(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->ipc_request_finish(ti);
}
