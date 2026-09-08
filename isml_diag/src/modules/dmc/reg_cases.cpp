#include "diag/module/DMCModule.h"

// dmc_reg_scan : To scan readable registers for one DMC controller.
// @input: args["range"] resolved by YAML defaults.
// @output: TestResult metrics include scanned_range and bad_register_count.
TestResult DMCModule::DmcRegScan(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "DMC implementation is not bound");
    }
    return impl_->DmcRegScan(ti);
}
