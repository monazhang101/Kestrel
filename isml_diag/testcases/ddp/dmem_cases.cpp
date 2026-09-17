#include "diag/modules/DDPModule.h"

// dmem_linkup_verify: Verify DDP device-memory link-up state.
// @input: args["link"] resolved by YAML defaults.
// @output: TestStatus; details are written to the testcase log.
TestStatus DDPModule::dmem_linkup_verify(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "DDP implementation is not bound");
    }
    return impl_->dmem_linkup_verify(ti);
}
