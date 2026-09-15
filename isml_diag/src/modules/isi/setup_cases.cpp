#include "diag/module/ISIModule.h"

// isi_setup : To initialize ISI link configuration for this ISI instance.
// @input: args["mode"] resolved by YAML defaults.
// @output: TestStatus; setup details are written to the testcase log.
TestStatus ISIModule::IsiSetup(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "ISI implementation is not bound");
    }
    return impl_->IsiSetup(ti);
}
