#include "diag/module/PMUModule.h"

// pmu_reg_read : To read one PMU register through the PMU register window.
// @input: args["offset"] register offset.
// @output: TestStatus; register details are written to the testcase log.
TestStatus PMUModule::PmuRegRead(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->PmuRegRead(ti);
}

// pmu_reg_write : To write one PMU register through the PMU register window.
// @input: args["offset"] register offset, args["value"] value to write.
// @output: TestStatus; register details are written to the testcase log.
TestStatus PMUModule::PmuRegWrite(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->PmuRegWrite(ti);
}

// pmu_reg_check : To read a PMU register and check it against an expected value.
// @input: args["offset"] register offset, args["expected"] expected value.
// @output: TestStatus; register details are written to the testcase log.
TestStatus PMUModule::PmuRegCheck(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_status(ti, "PMU implementation is not bound");
    }
    return impl_->PmuRegCheck(ti);
}
