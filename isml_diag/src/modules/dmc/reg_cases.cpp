#include "diag/module/DMCModule.h"

// dmc_reg_scan : To scan readable registers for one DMC controller.
// @input: args["range"] resolved by YAML defaults.
// @output: TestStatus; scan details are written to the testcase log.
TestStatus DMCModule::DmcRegScan(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "DMC implementation is not bound");
    }
    return impl_->DmcRegScan(ti);
}
