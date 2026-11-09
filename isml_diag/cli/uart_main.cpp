#include "diag/core/PhalBridge.h"
#include "diag/core/UartRepl.h"
#include "diag/device/TPUDevice.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
uint32_t number(const std::string& text)
{
    size_t used = 0;
    if (text.empty() || text[0] == '-') throw std::invalid_argument("expected an unsigned number");
    const auto value = std::stoull(text, &used, 0);
    if (used != text.size() || value > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("expected a 32-bit unsigned number");
    return uint32_t(value);
}
LogLevel log_level(const std::string& value)
{
    if (value == "error") return LogLevel::Error;
    if (value == "info") return LogLevel::Info;
    if (value == "flow" || value == "debug") return LogLevel::Flow;
    if (value == "regio" || value == "trace") return LogLevel::RegIo;
    throw std::invalid_argument("invalid --log-level");
}
std::filesystem::path default_policies(const char* program)
{
    std::error_code error;
    auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) executable = std::filesystem::absolute(program);
    return executable.parent_path() / "policies";
}
void usage()
{
    std::cout << "Usage: isml_uart <module_test_name> [--<argument> <value>]...\n"
              << "       isml_uart <module_test_name> -all\n"
              << "       isml_uart console [UART options]\n"
              << "       isml_uart list\n"
              << "  --target TPU0                one automatically selected CP210x UART\n"
              << "  --uart-console               enter RD/WR console after the testcase\n"
              << "  --uart-log                   save testcase/session PMU text to a log file\n"
              << "  --retry-count 3              retries after the initial send\n"
              << "  --retry-interval-ms 100      interval after an unanswered attempt\n"
              << "  --response-timeout-ms 5000   response wait per attempt\n"
              << "  --log-level error|info|flow|regio (default: info)\n"
              << "  --policy-dir <path>          parameter runs for -all\n"
              << "Examples:\n"
              << "  isml_uart console\n"
              << "  isml_uart pmu_uart_example --uart-log\n"
              << "  isml_uart pmu_uart_example --uart-console --uart-log\n"
              << "  isml_uart pmu_uart_read32_example --addr 0x02105060\n"
              << "  isml_uart pmu_uart_write32_example --addr 0x02105060 --value 0x12345678\n";
}

bool open_uart(UartBridge& bridge, const uart::Config& config, Logger& logger)
{
    if (!bridge.port().empty()) return true;
    std::string error;
    if (!bridge.open(config, &error)) {
        logger.error(error);
        return false;
    }
    logger.info("PMU UART port=" + bridge.port());
    if (!bridge.log_path().empty()) logger.info("PMU log=" + bridge.log_path());
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
        usage();
        return argc < 2 ? 1 : 0;
    }
    try {
        const std::string command = argv[1];
        uart::Config config;
        Logger logger;
        TestArgs args;
        bool all = false;
        bool uart_console = false;
        auto policy_dir = default_policies(argv[0]);
        for (int i = 2; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--uart-log") { config.capture_log = true; continue; }
            if (flag == "--uart-console") { uart_console = true; continue; }
            if (flag == "-all") { all = true; continue; }
            if (flag.rfind("--", 0) != 0 || flag.size() == 2)
                throw std::invalid_argument("invalid option " + flag);
            if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0)
                throw std::invalid_argument("missing value for " + flag);
            const std::string value = argv[++i];
            if (flag == "--target") {
                if (value != "TPU0") throw std::invalid_argument("isml_uart exposes only logical target TPU0");
            } else if (flag == "--retry-count") config.retry.retry_count = number(value);
            else if (flag == "--retry-interval-ms") config.retry.retry_interval_ms = number(value);
            else if (flag == "--response-timeout-ms") config.retry.response_timeout_ms = number(value);
            else if (flag == "--log-level") logger.set_level(log_level(value));
            else if (flag == "--policy-dir") {
                if (command == "console")
                    throw std::invalid_argument("console mode accepts only UART options");
                policy_dir = value;
            }
            else {
                auto name = flag.substr(2);
                std::replace(name.begin(), name.end(), '-', '_');
                if (!args.emplace(name, value).second) throw std::invalid_argument("duplicate argument " + flag);
            }
        }
        config.retry.validate();
        if (all && !args.empty()) throw std::invalid_argument("-all cannot be combined with testcase arguments");
        const bool console_only = command == "console";
        if (console_only) {
            if (uart_console)
                throw std::invalid_argument("console mode cannot be combined with --uart-console");
            if (all || !args.empty())
                throw std::invalid_argument("console mode accepts only UART options");
        }
        std::atomic_bool console_active{console_only};
        std::atomic_bool prompt_visible{false};
        if (console_only || uart_console) {
            config.on_log = [&](const std::string& text) {
                if (!console_active.load(std::memory_order_relaxed)) return;
                const auto redraw_prompt =
                    prompt_visible.load(std::memory_order_relaxed);
                std::string display;
                if (redraw_prompt) display += "\r\033[2K";
                display += text;
                if (text.empty() || text.back() != '\n') display += '\n';
                if (redraw_prompt) display += "uart> ";
                std::cout << display << std::flush;
            };
        }

        auto uart = std::make_shared<UartBridge>();
        const auto enter_console = [&](bool wait_for_first_command) {
            console_active.store(!wait_for_first_command, std::memory_order_relaxed);
            uart::run_repl(*uart, std::cin, std::cout, [&](bool waiting) {
                prompt_visible.store(waiting, std::memory_order_relaxed);
            }, [&] {
                console_active.store(true, std::memory_order_relaxed);
            });
            prompt_visible.store(false, std::memory_order_relaxed);
            console_active.store(false, std::memory_order_relaxed);
        };
        if (console_only) {
            if (!open_uart(*uart, config, logger)) return int(PHAL_STATUS_ERROR);
            enter_console(false);
            uart->close();
            const auto diagnostics = uart->diagnostics();
            if (!diagnostics.empty()) logger.error(diagnostics);
            return 0;
        }

        // Construct the same module/testcase catalog as the PCIe executable,
        // but with a synthetic device context. No PCI scan, VID/DID, BAR,
        // system-ready wait, or resource inventory access is performed here.
        DeviceContext device;
        device.tpu_type = TPUType::Atlas;
        device.target_name = "TPU0";
        device.logger = &logger;
        TPUDeviceConfig target_config;
        target_config.name = "TPU0";
        target_config.pcie_modules.push_back({0});
        target_config.pmu_modules.push_back({0});
        for (uint32_t index = 0; index < 4; ++index)
            target_config.ddp_modules.push_back({index});
        for (uint32_t index = 0; index < 8; ++index)
            target_config.isi_modules.push_back({index});
        TPUDevice tpu("TPU0", device, target_config, nullptr);
        auto names = tpu.get_registered_test_names();
        std::sort(names.begin(), names.end());
        if (command == "list") {
            if (argc != 2) throw std::invalid_argument("list takes no arguments");
            for (const auto& name : names) std::cout << name << '\n';
            return 0;
        }
        if (!std::binary_search(names.begin(), names.end(), command))
            throw std::invalid_argument("testcase is not packaged in isml_uart: " + command);
        const auto separator = command.find('_');
        auto* target = separator == std::string::npos
                     ? nullptr
                     : tpu.module(command.substr(0, separator), 0);
        if (target == nullptr)
            throw std::invalid_argument("testcase target is not available: " + command);
        std::vector<TestArgs> runs{args};
        if (all) {
            const auto catalog = load_testcase_policies((policy_dir / "testcases").string());
            const auto module_name = separator == std::string::npos
                                   ? std::string{}
                                   : command.substr(0, separator);
            const auto local_name = separator == std::string::npos
                                  ? std::string{}
                                  : command.substr(separator + 1);
            const auto module = catalog.find(module_name);
            if (module == catalog.end() || !module->second.count(local_name) ||
                module->second.at(local_name).runs.empty())
                throw std::invalid_argument("no parameter list configured for testcase: " + command);
            runs = module->second.at(local_name).runs;
        }
        PhalBridge phal;
        // UART-compatible cases use ti.phal_ctx. The unbound HAL keeps older
        // PCIe-only cases from dereferencing a null capability provider.
        HalContext uart_hal;
        int aggregate = 0;
        for (size_t index = 0; index < runs.size(); ++index) {
            if (all) std::cout << "[RUN " << index + 1 << '/' << runs.size() << "] TPU0::" << command << '\n';
            // Argument validation happens before opening a port. The serial
            // session stays open across every run; only PHAL ctx is per-case.
            const auto status = target->run_testcase_with(command, runs[index], &logger,
                [&](TestInfo& ti, const TestcaseDefinition& test) -> TestStatus {
                    std::string error;
                    if (!open_uart(*uart, config, logger)) return PHAL_STATUS_ERROR;
                    auto ctx = phal.create_uart_context(device, PhalProject::Atlas, &error, uart);
                    if (!ctx) { logger.error(error); return PHAL_STATUS_ERROR; }
                    ti.hal = &uart_hal;
                    ti.phal_ctx = ctx.get();
                    ti.transport = RegisterTransport::Uart;
                    return test.execute(ti);
                });
            std::cout << '[' << (status == PHAL_STATUS_OK ? "PASS" : test_status_name(status))
                      << "] TPU0::" << command << '\n';
            aggregate |= int(status);
        }
        if (uart_console && !uart->port().empty()) {
            enter_console(true);
        }
        uart->close();
        const auto diagnostics = uart->diagnostics();
        if (!diagnostics.empty()) logger.error(diagnostics);
        return aggregate;
    } catch (const std::exception& error) {
        std::cerr << "isml_uart: " << error.what() << '\n';
        return int(PHAL_STATUS_INVALID);
    }
}
