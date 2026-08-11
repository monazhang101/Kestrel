#include "TPUDevice.h"

namespace {

std::string get_arg(const TestArgs& args,
                    const std::string& key,
                    const std::string& default_value)
{
    auto it = args.find(key);
    return it == args.end() ? default_value : it->second;
}

} // namespace

TestResult TPUDevice::PmuReadSensor(const TestArgs& args)
{
    auto* pmu_regs = pmu_reg_addr();
    std::string sensor = get_arg(args, "sensor", "all");

    // Pseudocode:
    // 1. read PMU sensor status from pmu_regs
    // 2. read temperature/voltage/power values requested by args["sensor"]
    // 3. return observed values; thresholds are checked by Python/CLI policy
    (void)pmu_regs;

    return {"pmu_read_sensor", get_name(), true, {
        {"sensor", sensor},
        {"temperature_c", "42"},
        {"voltage_mv", "850"},
        {"power_w", "250"}
    }};
}
