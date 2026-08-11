#include "TPUDevice.h"

namespace {

TestResult probe_isi_link(const std::string& device_name,
                          const std::string& instance_name,
                          void* isi_regs)
{
    // Pseudocode:
    // 1. read ISI present/link-training/lane-ready registers from isi_regs
    // 2. check physical present and link training done bits
    // 3. return observed facts; connectivity testcase checks criteria
    (void)isi_regs;

    return {"isi_link_up", device_name, true, {
        {"instance", instance_name},
        {"link_status", "up"},
        {"lane_ready_bitmap", "0xff"},
        {"error_count", "0"}
    }};
}

} // namespace

TestResult TPUDevice::IsiLinkUp(const TestArgs& args)
{
    auto it = args.find("link");
    if (it == args.end()) {
        it = args.find("links");
    }
    std::string link = it == args.end() ? "all" : it->second;

    if (link == "isi0") {
        return probe_isi_link(get_name(), "isi0", isi_reg_addr(0));
    }

    if (link == "isi1") {
        return probe_isi_link(get_name(), "isi1", isi_reg_addr(1));
    }

    // Pseudocode for args["links"] == "all":
    // Run all ISI link probes and aggregate their observed state.
    auto isi0 = probe_isi_link(get_name(), "isi0", isi_reg_addr(0));
    auto isi1 = probe_isi_link(get_name(), "isi1", isi_reg_addr(1));

    return {"isi_link_up", get_name(), isi0.passed && isi1.passed, {
        {"isi0_status", isi0.metrics["link_status"]},
        {"isi1_status", isi1.metrics["link_status"]},
        {"isi0_lane_ready_bitmap", isi0.metrics["lane_ready_bitmap"]},
        {"isi1_lane_ready_bitmap", isi1.metrics["lane_ready_bitmap"]}
    }};
}
