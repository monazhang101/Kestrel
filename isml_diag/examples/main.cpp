#include "diag/device/DeviceManager.h"

#include <iostream>
#include <string>
#include <vector>

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

static void print_usage(const char* program)
{
    std::cout << "Usage:\n"
              << "  " << program << " discover [--backend phal|ihal|dryrun] [--tree]\n"
              << "  " << program << " link-status [--backend phal|ihal|dryrun] [--target <target>]\n"
              << "  " << program << " bar-read [--backend phal|ihal|dryrun] [--target <target>] [--bar <0|2|4>] [--offset <offset>]\n"
              << "  " << program << " bar-scan [--backend phal|ihal|dryrun] [--target <target>] [--bar <0|2|4>] [--offset <offset>] [--words <words>]\n"
              << "\n"
              << "Default backend: ihal\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " discover --tree\n"
              << "  " << program << " discover --backend phal --tree\n"
              << "  " << program << " link-status --target PCIE_0_0\n";
}

static void print_bar_map_status(const std::vector<BarMapping>& bars)
{
    for (const auto& bar : bars) {
        std::cout << " " << bar.name << "=" << (bar.mapped ? "ok" : "fail");
        std::cout << "(expected=0x" << std::hex << bar.expected_size
                  << " resource=0x" << bar.resource_size
                  << " mapped=0x" << bar.mapped_size;
        if (bar.mapped) {
            std::cout << " base=0x" << bar.device_base;
        }
        std::cout << std::dec;
        if (!bar.layout.empty()) {
            std::cout << " layout=";
            for (size_t i = 0; i < bar.layout.size(); ++i) {
                if (i != 0) {
                    std::cout << "|";
                }
                std::cout << bar.layout[i];
            }
        }
        if (!bar.error.empty()) {
            std::cout << " error=" << bar.error;
        }
        std::cout << ")";
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
                  << std::dec;
        print_bar_map_status(device.bars);
        std::cout << std::endl;
    }
}

static std::string get_option(int argc,
                              char** argv,
                              const std::string& name,
                              const std::string& default_value)
{
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) {
            return argv[i + 1];
        }
    }
    return default_value;
}

static bool has_flag(int argc, char** argv, const std::string& name)
{
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == name) {
            return true;
        }
    }
    return false;
}

static HalType parse_backend(const std::string& backend)
{
    if (backend.empty()) {
        return HalType::iHal;
    }
    if (backend == "ihal" || backend == "iHal") {
        return HalType::iHal;
    }
    if (backend == "dryrun" || backend == "phal" || backend == "pHal") {
        return HalType::pHal;
    }
    return HalType::iHal;
}

int main(int argc, char** argv)
{
    if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    DeviceManager device_manager(parse_backend(get_option(argc, argv, "--backend", "iHal")));
    device_manager.set_log_level(LogLevel::Trace);

    // Pseudocode: discover scans PCI devices, applies VID/DID policy,
    // mmaps BAR space, creates ATLAS parent devices, and registers child modules.
    DeviceTree tree = device_manager.discover();
    // Pass --backend phal to discover through the pHal/dry-run backend.

    const std::string command = argv[1];

    if (command == "discover") {
        print_discovery_summary(tree);

        if (has_flag(argc, argv, "--tree")) {
            std::cout << std::endl;
            std::cout << "=== Device Tree ===" << std::endl;
            device_manager.print_tree();
        }
        return 0;
    }

    if (command == "link-status") {
        auto target = get_option(argc, argv, "--target", "PCIE_0_0");
        print_test_result(device_manager.run_atomic_test(
            target, "pcie_link_status_get"));
        return 0;
    }

    if (command == "bar-read") {
        auto target = get_option(argc, argv, "--target", "PCIE_0_0");
        auto offset = get_option(argc, argv, "--offset", "0x0");
        auto bar = get_option(argc, argv, "--bar", "");
        TestArgs args = {
            {"offset", offset},
        };
        if (!bar.empty()) {
            args["bar_index"] = bar;
        }
        print_test_result(device_manager.run_atomic_test(
            target, "pcie_bar_read32", args));
        return 0;
    }

    if (command == "bar-scan") {
        auto target = get_option(argc, argv, "--target", "PCIE_0_0");
        auto offset = get_option(argc, argv, "--offset", "0x0");
        auto words = get_option(argc, argv, "--words", "16");
        auto bar = get_option(argc, argv, "--bar", "");
        TestArgs args = {
            {"offset", offset},
            {"words", words},
        };
        if (!bar.empty()) {
            args["bar_index"] = bar;
        }
        print_test_result(device_manager.run_atomic_test(
            target, "pcie_bar_scan32", args));
        return 0;
    }

    std::cerr << "Unknown command: " << command << std::endl;
    print_usage(argv[0]);
    return 1;
}
