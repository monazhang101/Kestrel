#include "diag/module/DDPModule.h"

// ddp_dmem_linkup_verify : To verify DDP device-memory link-up state.
// @input: args["link"] resolved by YAML defaults.
// @output: TestResult metrics include link and linkup_status.
TestResult DDPModule::DdpDmemLinkupVerify(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "DDP implementation is not bound");
    }
    return impl_->DdpDmemLinkupVerify(ti);
}
