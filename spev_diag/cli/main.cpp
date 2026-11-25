#include "diag/device/DeviceManager.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static int run_test(DeviceManager& device_manager,
                    const std::string& target,
                    const std::string& test,
                    const TestArgs& args = {})
{
    const auto status = device_manager.run_testcase(target, test, args);
    const std::string result = status == PHAL_STATUS_OK
                             ? "PASS"
                             : test_status_name(status);
    std::cout << "[" << result << "] "
              << target << "::" << test << std::endl;
    return static_cast<int>(status);
}

static void print_usage(const char* program)
{
    std::cout << "Usage:\n"
              << "  " << program << " discover [--policy-dir <path>] [--log-level <level>]\n"
              << "  " << program << " chip-reset-prepare --target TPU0 --output <snapshot.json>\n"
              << "  " << program << " chip-reset-recover --input <snapshot.json> [--timeout-ms 60000]\n"
              << "  " << program << " <module_test_name> [--target TPU0] [--<argument> <value>]...\n"
              << "  " << program << " <module_test_name> [--target TPU0] -all\n"
              << "\n"
              << "Default target: TPU0\n"
              << "Global logging: --log-level error|info|flow|regio (default: info)\n"
              << "Policy directory: --policy-dir <path> (default: beside the executable)\n"
              << "DMA: pcie_dma_data_transfer [--drv spev_drv|none]\n"
              << "     [--direction h2d|d2h|h2i|i2h|h2s|s2h|d2i2d|d2s2d]\n"
              << "     [--size-bytes <bytes>] [--pattern zero|incremental|random] [--timeout-ms <ms>]\n"
              << "     Defaults: drv=spev_drv, h2d, 4096 bytes (4 KiB), random, 1000 ms.\n"
              << "     H2D: one DMA transfer, then one MMIO readback for verification.\n"
              << "     Use -all to run every parameter line from the testcase policy.\n"
              << "DMA concurrency: pcie_dma_concurrent_transfer [--drv spev_drv|none]\n"
              << "     [--job-count <total jobs>] [--size-bytes <multiple of 32>]\n"
              << "     Uses a sliding window of at most 255 in-flight jobs.\n"
              << "     [--pattern zero|incremental|random] [--timeout-ms <whole batch ms>]\n"
              << "DMA threaded stress: pcie_dma_threaded_stress [--drv spev_drv|none]\n"
              << "     [--iterations <count>] [--size-bytes <multiple of 32>] [--timeout-ms <per transfer ms>]\n"
              << "     Runs concurrent H2D, D2H, and DMEM MMIO workers with one shared DMA engine.\n"
              << "CP:  pcie_cp_queue_data_transfer [--direction h2sram|sram2h|d2sram|sram2d]\n"
              << "     [--queue mem0|mem1|mem2|mem3|comp0|comp1|comp2|comp3]\n"
              << "     [--size-bytes <multiple of 128>] [--sram-bank 0|1|2|3]\n"
              << "     [--pattern zero|incremental|random] [--timeout-ms <ms>]\n"
              << "     Defaults: h2sram, mem3, 512 bytes, HQC SRAM bank 0, random, 1000 ms.\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " discover\n"
              << "  sudo " << program << " chip-reset-prepare --target TPU0 --output before-reset.json\n"
              << "  sudo " << program << " chip-reset-recover --input before-reset.json\n"
              << "  " << program << " pcie_bar_read32 --target TPU0 --bar-index 0 --offset 0x20f80\n"
              << "  " << program << " pcie_dma_data_transfer\n"
              << "  " << program << " pcie_dma_data_transfer --target TPU0 --drv none --direction h2d --size-bytes 0x10000 --pattern incremental --timeout-ms 30000\n"
              << "  " << program << " pcie_dma_concurrent_transfer --target TPU0 --job-count 32 --size-bytes 4096 --timeout-ms 30000\n"
              << "  " << program << " pcie_dma_threaded_stress --target TPU0 --iterations 100 --size-bytes 4096 --timeout-ms 30000\n"
              << "  " << program << " pcie_sequential_aperture_mapping --target TPU0 --log-level regio\n";
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
        for (const auto& property : device.properties) {
            std::cout << "  " << property.first << "=" << property.second
                      << std::endl;
        }
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

static bool validate_discover_args(int argc, char** argv, std::string& error)
{
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option != "--log-level" && option != "--policy-dir") {
            error = "invalid discover option: " + option;
            return false;
        }
        if (i + 1 >= argc) {
            error = option + " requires a value";
            return false;
        }
        ++i;
    }
    return true;
}

static bool validate_lifecycle_args(int argc,
                                    char** argv,
                                    const std::string& command,
                                    std::string& error)
{
    const std::vector<std::string> allowed = command == "chip-reset-prepare"
        ? std::vector<std::string>{"--target", "--output", "--policy-dir", "--log-level"}
        : std::vector<std::string>{"--input", "--timeout-ms", "--policy-dir", "--log-level"};
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (std::find(allowed.begin(), allowed.end(), option) == allowed.end()) {
            error = "invalid " + command + " option: " + option;
            return false;
        }
        if (i + 1 >= argc) {
            error = option + " requires a value";
            return false;
        }
        ++i;
    }
    const auto required = command == "chip-reset-prepare" ? "--output" : "--input";
    if (get_option(argc, argv, required, "").empty()) {
        error = std::string(required) + " is required";
        return false;
    }
    return true;
}

static bool parse_timeout_ms(const std::string& value,
                             uint32_t& timeout_ms,
                             std::string& error)
{
    try {
        size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size() || parsed > 3600000) {
            throw std::out_of_range("timeout");
        }
        timeout_ms = static_cast<uint32_t>(parsed);
        return true;
    } catch (const std::exception&) {
        error = "--timeout-ms must be an integer between 0 and 3600000";
        return false;
    }
}

static bool parse_test_args(int argc,
                            char** argv,
                            TestArgs& args,
                            bool& run_all,
                            std::string& error)
{
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "-all") {
            run_all = true;
            continue;
        }
        if (option == "--target" || option == "--log-level" ||
            option == "--policy-dir") {
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

static LogLevel parse_log_level(const std::string& level)
{
    if (level == "error") {
        return LogLevel::Error;
    }
    if (level == "flow" || level == "debug") {
        return LogLevel::Flow;
    }
    if (level == "regio" || level == "trace") {
        return LogLevel::RegIo;
    }
    return LogLevel::Info;
}

static std::string default_policy_dir(const char* program)
{
    std::error_code error;
    const auto proc_executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return (proc_executable.parent_path() / "policies").string();
    error.clear();
    const auto executable = std::filesystem::absolute(program, error);
    const auto directory = error ? std::filesystem::current_path()
                                 : executable.parent_path();
    return (directory / "policies").string();
}

int main(int argc, char** argv)
{
    if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    const std::string command = argv[1];
    const bool lifecycle_command =
        command == "chip-reset-prepare" || command == "chip-reset-recover";
    if (command == "discover") {
        std::string error;
        if (!validate_discover_args(argc, argv, error)) {
            std::cerr << error << std::endl;
            return 1;
        }
    } else if (lifecycle_command) {
        std::string error;
        if (!validate_lifecycle_args(argc, argv, command, error)) {
            std::cerr << error << std::endl;
            return 1;
        }
    }

    const auto policy_dir = get_option(
        argc, argv, "--policy-dir", default_policy_dir(argv[0]));
    DeviceManager device_manager(policy_dir);

    device_manager.set_log_level(
        parse_log_level(get_option(argc, argv, "--log-level", "info")));

    if (command == "chip-reset-prepare") {
        const auto target = get_option(argc, argv, "--target", "TPU0");
        const auto output = get_option(argc, argv, "--output", "");
        std::string error;
        const auto status = device_manager.prepare_before_reset(target, output, &error);
        if (status != PHAL_STATUS_OK) {
            std::cerr << error << std::endl;
            return static_cast<int>(status);
        }
        std::cout << "Prepared " << target << " for manual chip reset.\n"
                  << "Recovery snapshot: " << output << std::endl;
        return 0;
    }
    if (command == "chip-reset-recover") {
        uint32_t timeout_ms = 60000;
        std::string error;
        if (!parse_timeout_ms(get_option(argc, argv, "--timeout-ms", "60000"),
                              timeout_ms, error)) {
            std::cerr << error << std::endl;
            return 1;
        }
        const auto input = get_option(argc, argv, "--input", "");
        const auto status = device_manager.recover_after_reset(
            input, timeout_ms, &error);
        if (status != PHAL_STATUS_OK) {
            std::cerr << error << std::endl;
            return static_cast<int>(status);
        }
        std::cout << "Recovered device and rebuilt framework mappings from "
                  << input << std::endl;
        return 0;
    }

    // Discovery scans PCI devices, applies VID/DID policy, maps BAR space,
    // creates TPU targets, and registers their child modules.
    DeviceTree tree = device_manager.discover();
    if (command == "discover") {
        print_discovery_summary(tree);
        std::cout << std::endl;
        std::cout << "=== Device Tree ===" << std::endl;
        device_manager.print_tree();
        return 0;
    }

    const auto target = get_option(argc, argv, "--target", "TPU0");

    TestArgs args;
    bool run_all = false;
    std::string error;
    if (!parse_test_args(argc, argv, args, run_all, error)) {
        std::cerr << error << std::endl;
        return 1;
    }
    if (run_all && !args.empty()) {
        std::cerr << "-all cannot be combined with testcase arguments" << std::endl;
        return 1;
    }
    if (run_all) {
        const auto runs = device_manager.get_testcase_runs(command);
        if (runs.empty()) {
            std::cerr << "no parameter list configured for testcase: " << command << std::endl;
            return static_cast<int>(PHAL_STATUS_INVALID);
        }

        int aggregate = static_cast<int>(PHAL_STATUS_OK);
        size_t passed = 0;
        for (size_t index = 0; index < runs.size(); ++index) {
            std::cout << "[RUN " << index + 1 << "/" << runs.size() << "] "
                      << target << "::" << command << std::endl;
            const auto result = run_test(device_manager, target, command, runs[index]);
            aggregate |= result;
            if (result == static_cast<int>(PHAL_STATUS_OK)) ++passed;
        }
        std::cout << "[SUMMARY] completed=" << runs.size()
                  << " passed=" << passed
                  << " failed=" << runs.size() - passed << std::endl;
        return aggregate;
    }
    return run_test(device_manager, target, command, args);
}
