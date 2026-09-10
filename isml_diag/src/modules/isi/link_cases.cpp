#include "diag/module/ISIModule.h"

// isi_linkup : To check whether this ISI link is physically present and link-up.
// @input: none.
// @output: TestStatus; link details are written to the testcase log.
TestStatus ISIModule::IsiLinkup(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "ISI implementation is not bound");
    }
    return impl_->IsiLinkup(ti);
}
