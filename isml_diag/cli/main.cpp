#include "diag/device/DeviceManager.h"

#include <iostream>
#include <string>
#include <vector>

static int run_test(DeviceManager& device_manager,
                    const std::string& target,
                    const std::string& test,
                    const TestArgs& args = {})
{
    const auto status = device_manager.run_testcase(target, test, args);
    const std::string result = status == TestStatus::OK
                             ? "PASS"
                             : test_status_name(status);
    std::cout << "[" << result << "] "
              << target << "::" << test << std::endl;
    return static_cast<int>(status);
}

static void print_usage(const char* program)
{
    std::cout << "Usage:\n"
              << "  " << program << " discover [--backend phal|ihal|dryrun] [--tree]\n"
              << "  " << program << " <test_name> --target <target> [--<argument> <value>]...\n"
              << "\n"
              << "Default backend: ihal\n"
              << "Global logging: --log-level error|info|debug|trace (default: info)\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " discover --tree\n"
              << "  " << program << " discover --backend phal --tree\n"
              << "  " << program << " bar_read32 --target PCIE_0_0 --offset 0x20f80\n"
              << "  " << program << " dma_data_transfer --target PCIE_0_0 --direction h2d --pattern incremental\n"
              << "  " << program << " sequential_aperture_mapping --target PCIE_0_0 --log-level debug\n";
}

static void print_bar_map_status(const std::vector<BarMapping>& bars)
{
    if (bars.empty()) {
        return;
    }

    std::cout << "  BAR mappings:" << std::endl;
    for (const auto& bar : bars) {
        std::cout << "    " << bar.name
                  << " [" << (bar.mapped ? "ok" : "fail") << "]" << std::endl;
        std::cout << "      expected_size = 0x" << std::hex << bar.expected_size << std::endl
                  << "      resource_size = 0x" << bar.resource_size << std::endl
                  << "      mapped_size   = 0x" << bar.mapped_size << std::endl;
        if (bar.mapped) {
            std::cout << "      device_base   = 0x" << bar.device_base << std::endl;
        }
        std::cout << std::dec;
        if (!bar.layout.empty()) {
            std::cout << "      layout:" << std::endl;
            for (const auto& item : bar.layout) {
                std::cout << "        - " << item << std::endl;
            }
        }
        if (!bar.error.empty()) {
            std::cout << "      error         = " << bar.error << std::endl;
        }
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
        print_bar_map_status(device.bars);
        if (!device.bars.empty()) {
            std::cout << std::endl;
        }
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

static bool parse_test_args(int argc,
                            char** argv,
                            TestArgs& args,
                            std::string& error)
{
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--target" || option == "--backend" ||
            option == "--log-level") {
            if (i + 1 >= argc) {
                error = option + " requires a value";
                return false;
            }
            ++i;
            continue;
        }
        if (option.rfind("--", 0) != 0 || option.size() <= 2) {
            error = "invalid test argument option: " + option;
            return false;
        }
        if (i + 1 >= argc) {
            error = option + " requires a value";
            return false;
        }

        auto argument_name = option.substr(2);
        for (auto& character : argument_name) {
            if (character == '-') {
                character = '_';
            }
        }
        args[argument_name] = argv[++i];
    }
    return true;
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

    // Discovery scans PCI devices, applies VID/DID policy, maps BAR space,
    // creates TPU targets, and registers their child modules.
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

    const auto target = get_option(argc, argv, "--target", "");
    if (target.empty()) {
        std::cerr << command << " requires --target" << std::endl;
        return 1;
    }

    TestArgs args;
    std::string error;
    if (!parse_test_args(argc, argv, args, error)) {
        std::cerr << error << std::endl;
        return 1;
    }
    return run_test(device_manager, target, command, args);
}
