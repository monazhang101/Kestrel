#include "diag/module/DMCModule.h"

// dmc_status_check : To check one DDP memory-controller status block.
// @input: none.
// @output: TestStatus; details are written to the testcase log.
TestStatus DMCModule::DmcStatusCheck(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "DMC implementation is not bound");
    }
    return impl_->DmcStatusCheck(ti);
}
