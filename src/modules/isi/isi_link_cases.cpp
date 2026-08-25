#include "diag/module/ISIModule.h"

// isi_linkup : To check whether this ISI link is physically present and link-up.
// @input: none.
// @output: TestResult metrics include link_id, link_status, lane_ready_bitmap, and error_count.
TestResult ISIModule::IsiLinkup(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "ISI implementation is not bound");
    }
    return impl_->IsiLinkup(ti);
}
