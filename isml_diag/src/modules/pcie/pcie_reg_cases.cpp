#include "diag/module/PCIeModule.h"

// pcie_bar_read32 : To read one 32-bit word from this PCIe module BAR window.
// @input: args["offset"] offset relative to the PCIe module register window.
// @output: TestResult metrics include offset, absolute_bar_offset, and value.
TestResult PCIeModule::pcie_bar_read32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_read32(ti);
}

// pcie_bar_scan32 : To scan a small range of 32-bit words from this PCIe module BAR window.
// @input: args["offset"] start offset, args["words"] number of 32-bit words.
// @output: TestResult metrics include offset, words, and word_N values.
TestResult PCIeModule::pcie_bar_scan32(TestInfo& ti)
{
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }
    return impl_->bar_scan32(ti);
}
