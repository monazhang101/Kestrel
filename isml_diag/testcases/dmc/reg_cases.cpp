#include "diag/modules/DMCModule.h"

// reg_scan: Scan readable registers for one DMC controller.
// @input: args["range"] resolved by YAML defaults.
// @output: TestStatus; scan details are written to the testcase log.
TestStatus DMCModule::reg_scan(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "DMC implementation is not bound");
    }
    return impl_->reg_scan(ti);
}
