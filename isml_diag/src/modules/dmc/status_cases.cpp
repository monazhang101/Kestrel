#include "diag/module/DMCModule.h"

// dmc_status_check : To check one DDP memory-controller status block.
// @input: none.
// @output: TestResult metrics include ddp_id, controller_id, and status.
TestResult DMCModule::DmcStatusCheck(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "DMC implementation is not bound");
    }
    return impl_->DmcStatusCheck(ti);
}
