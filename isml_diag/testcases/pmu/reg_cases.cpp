#include "diag/modules/PMUModule.h"

// reg_read: Read one PMU register through the PMU register window.
// @input: args["offset"] register offset.
// @output: TestStatus; register details are written to the testcase log.
TestStatus PMUModule::reg_read(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->reg_read(ti);
}

// reg_write: Write one PMU register through the PMU register window.
// @input: args["offset"] register offset, args["value"] value to write.
// @output: TestStatus; register details are written to the testcase log.
TestStatus PMUModule::reg_write(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->reg_write(ti);
}

// reg_check: Read a PMU register and check it against an expected value.
// @input: args["offset"] register offset, args["expected"] expected value.
// @output: TestStatus; register details are written to the testcase log.
TestStatus PMUModule::reg_check(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->reg_check(ti);
}
