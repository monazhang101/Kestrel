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

    print_discovery_summary(tree);

    std::cout << std::endl;
    std::cout << "=== Device Tree ===" << std::endl;
    device_manager.print_tree();

    std::cout << std::endl;
    std::cout << "=== CLI One-shot Simulation ===" << std::endl;

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0", "identify"));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0.PCIE_0", "pcie_enum_check"));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0.PCIE_0", "pcie_link_status_check", {
            {"expected_speed", "gen5"},
            {"expected_width", "x16"},
        }));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0.PCIE_0", "pcie_dma_data_transfer", {
            {"direction", "h2d"},
            {"size_bytes", "4096"},
            {"pattern", "incremental"},
        }));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0", "soc_gpio_read", {
            {"pin", "3"},
        }));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0.PMU_0", "pmu_reg_read", {
            {"offset", "0x40"},
        }));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0.ISI_1", "isi_linkup"));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_0.DDP_0", "ddp_dvsec_verify"));

    print_test_result(device_manager.run_atomic_test(
        "ATLAS_1.PCIE_0", "pcie_dma_data_transfer", {
            {"direction", "both"},
            {"size_bytes", "1048576"},
        }));

    // Error scenario: target exists, but this atomic test is not registered there.
    print_test_result(device_manager.run_atomic_test(
        "ATLAS_1.PMU_0", "pcie_dma_data_transfer"));

    // Error scenario: unknown target name.
    print_test_result(device_manager.run_atomic_test(
        "ATLAS_9", "identify"));

    return 0;
}
