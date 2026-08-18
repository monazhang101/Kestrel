#include "diag/device/DeviceManager.h"

#include <iostream>

static void print_test_result(const TestResult& result)
{
    std::cout << "["
              << (result.passed ? "PASS" : "FAIL")
              << "] " << result.target_name
              << "::" << result.test_name
              << std::endl;

    for (const auto& metric : result.metrics) {
        std::cout << "    " << metric.first << " = " << metric.second << std::endl;
    }

    if (!result.error_description.empty()) {
        std::cout << "    error_description = " << result.error_description << std::endl;
        std::cout << "    error_details = " << result.error_details << std::endl;
    }
}

static void print_discovery_summary(const DeviceTree& tree)
{
    std::cout << "=== Discovered Devices ===" << std::endl;

    for (const auto& device : tree.devices) {
        std::cout << device.name
                  << " type=" << device.type
                  << " parent=" << device.parent
                  << " bdf=" << device.bdf
                  << " vid=0x" << std::hex << device.vendor_id
                  << " did=0x" << device.device_id
                  << std::dec << std::endl;
    }
}

int main()
{
    DeviceManager device_manager;
    device_manager.set_log_level(LogLevel::Trace);

    // Pseudocode: discover scans PCI devices, applies BDF/VID/DID policy,
    // mmaps BAR space, creates ATLAS parent devices, and registers child modules.
    DeviceTree tree = device_manager.discover();
    // Default is iHal. Pass pHal if needed.
    // DeviceTree tree = device_manager.discover(HalType::pHal);

    print_discovery_summary(tree);

    std::cout << std::endl;
    std::cout << "=== Device Tree ===" << std::endl;
    device_manager.print_tree();

    std::cout << std::endl;
    std::cout << "=== CLI One-shot Simulation ===" << std::endl;

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0.PCIE_0", "pcie_link_status_get"));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0.PCIE_0", "pcie_dma_data_transfer", {
            {"direction", "h2d"},
            {"size_bytes", "4096"},
            {"pattern", "incremental"},
        }));

    return 0;
}
