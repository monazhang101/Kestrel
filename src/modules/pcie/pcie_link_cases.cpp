#include "diag/module/PCIeModule.h"

// pcie_link_status_get : To get the current PCIe negotiated speed and link width.
// @input: none
// @output: TestResult metrics include current_speed and current_width.
TestResult PCIeModule::pcie_link_status_get(TestInfo& ti)
{
    (void)reg_base_;
    (void)reg_size_;
    if (impl_ == nullptr) {
        return make_unimplemented_result(ti, "PCIe implementation is not bound");
    }

    auto status = impl_->link_status_get();
    TestMetrics metrics = status.metrics;
    metrics["current_speed"] = status.current_speed;
    metrics["current_width"] = status.current_width;
    return {
        "pcie_link_status_get",
        get_name(),
        status.ok,
        metrics,
        status.ok ? "" : status.error
    };
}
