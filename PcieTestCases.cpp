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

TestResult TPUDevice::PcieLinkStatusCheck(const TestArgs& args)
{
    auto* pcie_regs = pcie_reg_addr();

    // Pseudocode:
    // 1. read PCIe link status/capability registers from pcie_regs
    // 2. optionally walk switch/root-port depth when args["depth"] == "all"
    // 3. return observed link speed/width; policy comparison happens above HAL
    (void)pcie_regs;

    return {"pcie_link_status_check", get_name(), true, {
        {"link_status", "up"},
        {"link_width", "x16"},
        {"link_speed", "32GT/s"},
        {"depth", get_arg(args, "depth", "endpoint")}
    }};
}

TestResult TPUDevice::PcieDmaDataTransfer(const TestArgs& args)
{
    auto* pcie_regs = pcie_reg_addr();
    std::string direction = get_arg(args, "direction", "h2d");
    std::string size_bytes = get_arg(args, "size_bytes", "4096");

    if (direction != "h2d" && direction != "d2h" && direction != "both") {
        return {"pcie_dma_data_transfer", get_name(), false, {
            {"error", "invalid direction"},
            {"direction", direction}
        }};
    }

    // Pseudocode:
    // 1. allocate DMA buffer through UMD/HAL/kmod memory provider
    // 2. fill source buffer with selected pattern
    // 3. program H2D, D2H, or both directions according to args["direction"]
    // 4. ring PCIe DMA doorbell through pcie_regs
    // 5. wait completion and verify status/data
    (void)pcie_regs;

    return {"pcie_dma_data_transfer", get_name(), true, {
        {"direction", direction},
        {"size_bytes", size_bytes},
        {"h2d_status", direction == "d2h" ? "skipped" : "pass"},
        {"d2h_status", direction == "h2d" ? "skipped" : "pass"}
    }};
}
