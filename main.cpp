#include "DeviceManager.h"

#include <iostream>

static void print_test_result(const TestResult& result)
{
    std::cout << "["
              << (result.passed ? "PASS" : "FAIL")
              << "] " << result.device_name
              << "::" << result.test_name
              << std::endl;

    for (const auto& metric : result.metrics) {
        std::cout << "    " << metric.first << " = " << metric.second << std::endl;
    }
}

static void print_discovery_summary(const DeviceTree& tree)
{
    std::cout << "=== Discovered Devices ===" << std::endl;

    for (const auto& device : tree.devices) {
        std::cout << device.name
                  << " bdf=" << device.bdf
                  << " vid=0x" << std::hex << device.vendor_id
                  << " did=0x" << device.device_id
                  << std::dec << std::endl;
    }
}

int main()
{
    DeviceManager device_manager;
    DeviceTree tree = device_manager.discover();

    print_discovery_summary(tree);

    std::cout << std::endl;
    std::cout << "=== Device Tree ===" << std::endl;
    device_manager.print_tree();

    std::cout << std::endl;
    std::cout << "=== CLI One-shot Simulation ===" << std::endl;

    print_test_result(device_manager.run_atomic_test(
        "TPU_0", "identify"));

    print_test_result(device_manager.run_atomic_test(
        "TPU_0", "pcie_link_status_check", {
            {"depth", "all"},
        }));

    print_test_result(device_manager.run_atomic_test(
        "TPU_0", "isi_link_up", {
            {"links", "all"},
        }));

    print_test_result(device_manager.run_atomic_test(
        "TPU_0", "pcie_dma_data_transfer", {
            {"direction", "h2d"},
            {"size_bytes", "4096"},
        }));

    print_test_result(device_manager.run_atomic_test(
        "TPU_1", "pcie_dma_data_transfer", {
            {"direction", "both"},
            {"size_bytes", "1048576"},
        }));

    print_test_result(device_manager.run_atomic_test(
        "TPU_1", "pmu_read_sensor", {
            {"sensor", "voltage"},
        }));

    // Error scenario: unknown atomic test name.
    print_test_result(device_manager.run_atomic_test(
        "TPU_1", "unknown_test"));

    // Error scenario: unknown device target.
    print_test_result(device_manager.run_atomic_test(
        "TPU_9", "identify"));

    return 0;
}
