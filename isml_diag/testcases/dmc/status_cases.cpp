#include "diag/modules/DMCModule.h"

// status_check: Check one DDP memory-controller status block.
// @input: none.
// @output: TestStatus; details are written to the testcase log.
TestStatus DMCModule::status_check(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "DMC implementation is not bound");
    }
    return impl_->status_check(ti);
}
