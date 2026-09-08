#include "diag/module/ISIModule.h"

// isi_setup : To initialize ISI link configuration for this ISI instance.
// @input: args["mode"] resolved by YAML defaults.
// @output: TestResult metrics include link_id, mode, and setup_status.
TestResult ISIModule::IsiSetup(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "ISI implementation is not bound");
    }
    return impl_->IsiSetup(ti);
}
