#include "diag/core/PhalBridge.h"
#include "diag/device/TPUDevice.h"
#include "diag/implementer/Implementer.h"

#include <algorithm>
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
              << "       isml_uart list\n"
              << "  --target TPU0                one automatically selected CP210x UART\n"
              << "  --uart-log                   save PMU text to logs/pmu_uart_<session>.log\n"
              << "  --retry-count 3              retries after the initial send\n"
              << "  --retry-interval-ms 100      interval after an unanswered attempt\n"
              << "  --response-timeout-ms 5000   response wait per attempt\n"
              << "  --log-level error|info|flow|regio (default: info)\n"
              << "  --policy-dir <path>          parameter runs for -all\n"
              << "Examples:\n"
              << "  isml_uart pmu_uart_example --uart-log\n"
              << "  isml_uart pmu_uart_read32_example --addr 0x02105060\n"
              << "  isml_uart pmu_uart_write32_example --addr 0x02105060 --value 0x12345678\n";
}

TPUDeviceConfig uart_target_config()
{
    TPUDeviceConfig config;
    config.name = "TPU0";
    config.tpu_index = 0;
    config.pcie_modules.push_back({0});
    config.pmu_modules.push_back({0});
    for (uint32_t index = 0; index < 4; ++index) {
        config.ddp_modules.push_back({index});
    }
    for (uint32_t index = 0; index < 8; ++index) {
        config.isi_modules.push_back({index});
    }
    return config;
}

std::unique_ptr<TPUDevice> create_uart_target(DeviceContext& device)
{
    auto implementer = std::make_shared<Implementer>(TPUType::Atlas);
    device.memory_regions = implementer->memory_regions();
    return std::make_unique<TPUDevice>(
        "TPU0", device, uart_target_config(), implementer);
}

void collect_test_names(const TestTarget& target,
                        std::vector<std::string>& names)
{
    const auto own = target.get_registered_test_names();
    names.insert(names.end(), own.begin(), own.end());
    for (const auto* child : target.child_targets()) {
        if (child != nullptr) collect_test_names(*child, names);
    }
}

TestTarget* find_uart_target(TPUDevice& device, const std::string& test_name)
{
    const auto separator = test_name.find('_');
    if (separator == std::string::npos) return nullptr;
    const auto module_type = test_name.substr(0, separator);
    return device.module(module_type, 0);
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
        auto policy_dir = default_policies(argv[0]);
        for (int i = 2; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--uart-log") { config.capture_log = true; continue; }
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
            else if (flag == "--policy-dir") policy_dir = value;
            else {
                auto name = flag.substr(2);
                std::replace(name.begin(), name.end(), '-', '_');
                if (!args.emplace(name, value).second) throw std::invalid_argument("duplicate argument " + flag);
            }
        }
        config.retry.validate();
        if (all && !args.empty()) throw std::invalid_argument("-all cannot be combined with testcase arguments");
        // Construct the same module/testcase catalog as the PCIe executable,
        // but with a synthetic device context. No PCI scan, VID/DID, BAR,
        // system-ready wait, or resource inventory access is performed here.
        DeviceContext device;
        device.tpu_type = TPUType::Atlas;
        device.target_name = "TPU0";
        device.logger = &logger;
        auto tpu = create_uart_target(device);
        auto names = std::vector<std::string>{};
        collect_test_names(*tpu, names);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        if (command == "list") {
            if (argc != 2) throw std::invalid_argument("list takes no arguments");
            for (const auto& name : names) std::cout << name << '\n';
            return 0;
        }
        if (!std::binary_search(names.begin(), names.end(), command))
            throw std::invalid_argument("testcase is not packaged in isml_uart: " + command);
        std::vector<TestArgs> runs{args};
        if (all) {
            const auto catalog = load_testcase_policies((policy_dir / "testcases").string());
            const auto separator = command.find('_');
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
        auto uart = std::make_shared<UartBridge>();
        PhalBridge phal;
        // The object is deliberately unbound: it gives PCIe-dependent cases a
        // safe capability check through ti.hal without performing discovery.
        HalContext uart_hal;
        int aggregate = 0;
        for (size_t index = 0; index < runs.size(); ++index) {
            if (all) std::cout << "[RUN " << index + 1 << '/' << runs.size() << "] TPU0::" << command << '\n';
            // Argument validation happens before opening a port. The serial
            // session stays open across every run; only PHAL ctx is per-case.
            auto* target = find_uart_target(*tpu, command);
            if (target == nullptr) {
                throw std::invalid_argument("testcase target is not available: " + command);
            }
            const auto status = target->run_testcase_with(command, runs[index], &logger,
                [&](TestInfo& ti, const TestcaseDefinition& test) -> TestStatus {
                    std::string error;
                    if (uart->port().empty()) {
                        if (!uart->open(config, &error)) { logger.error(error); return PHAL_STATUS_ERROR; }
                        logger.info("PMU UART port=" + uart->port());
                        if (!uart->log_path().empty()) logger.info("PMU log=" + uart->log_path());
                    }
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
        uart->close();
        const auto diagnostics = uart->diagnostics();
        if (!diagnostics.empty()) logger.error(diagnostics);
        return aggregate;
    } catch (const std::exception& error) {
        std::cerr << "isml_uart: " << error.what() << '\n';
        return int(PHAL_STATUS_INVALID);
    }
}
