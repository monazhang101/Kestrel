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
              << "  " << program << " sequential-aperture-mapping [--backend phal|ihal|dryrun] [--target <target>]\n"
              << "  " << program << " isi-pcie-aperture-context [--backend phal|ihal|dryrun] [--target <target>] [--bar <2|4>] [--aperture <id>]\n"
              << "  " << program << " isi-common-devmem-read [--backend phal|ihal|dryrun] [--target <target>] [--bar <2|4>] [--aperture <id>] [--identity <id>] [--target-addr <addr>] [--size <bytes>] [--bar-offset <offset>] [--offset <offset>]\n"
              << "\n"
              << "Default backend: ihal\n"
              << "Global logging: --log-level error|info|debug|trace (default: info)\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " discover --tree\n"
              << "  " << program << " discover --backend phal --tree\n"
              << "  " << program << " link-status --target PCIE_0_0\n"
              << "  " << program << " sequential-aperture-mapping --target PCIE_0_0 --log-level debug\n"
              << "  " << program << " isi-pcie-aperture-context --target ISI_0_0\n"
              << "  " << program << " isi-common-devmem-read --target ISI_0_0\n";
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

static LogLevel parse_log_level(const std::string& level)
{
    if (level == "error") {
        return LogLevel::Error;
    }
    if (level == "debug") {
        return LogLevel::Debug;
    }
    if (level == "trace") {
        return LogLevel::Trace;
    }
    return LogLevel::Info;
}

int main(int argc, char** argv)
{
    if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    DeviceManager device_manager(parse_backend(get_option(argc, argv, "--backend", "iHal")));

    device_manager.set_log_level(
        parse_log_level(get_option(argc, argv, "--log-level", "info")));

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

    if (command == "sequential-aperture-mapping") {
        auto target = get_option(argc, argv, "--target", "PCIE_0_0");
        print_test_result(device_manager.run_atomic_test(
            target, "sequential_aperture_mapping"));
        return 0;
    }

    if (command == "isi-pcie-aperture-context") {
        auto target = get_option(argc, argv, "--target", "ISI_0_0");
        TestArgs args = {
            {"bar_index", get_option(argc, argv, "--bar", "4")},
            {"aperture_index", get_option(argc, argv, "--aperture", "0")},
        };
        print_test_result(device_manager.run_atomic_test(
            target, "isi_pcie_aperture_context", args));
        return 0;
    }

    if (command == "isi-common-devmem-read") {
        auto target = get_option(argc, argv, "--target", "ISI_0_0");
        TestArgs args = {
            {"bar_index", get_option(argc, argv, "--bar", "4")},
            {"aperture_index", get_option(argc, argv, "--aperture", "0")},
            {"identity", get_option(argc, argv, "--identity", "0")},
            {"target_addr", get_option(argc, argv, "--target-addr", "0x10000000")},
            {"aperture_size", get_option(argc, argv, "--size", "0x100000")},
            {"bar_offset", get_option(argc, argv, "--bar-offset", "0")},
            {"offset", get_option(argc, argv, "--offset", "0")},
        };
        print_test_result(device_manager.run_atomic_test(
            target, "isi_common_devmem_read", args));
        return 0;
    }

    std::cerr << "Unknown command: " << command << std::endl;
    print_usage(argv[0]);
    return 1;
}
