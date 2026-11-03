// Linux-only integration: real termios/Unix sockets/PHAL callbacks, with a PTY
// standing in for the board. No USB device, firmware or Python is required.
#include "uart/UartConsole.h"
#include "uart/UartPosix.h"
#include "uart/UartRpc.h"
#include "diag/core/HalContext.h"
#include "diag/device/TPUDevice.h"

extern "C" {
#include "cobs.h"
}

#include <cstdio>
#include <deque>
#include <future>
#include <iostream>
#include <thread>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
using namespace std::chrono_literals;

namespace {
struct Command {
    uint64_t session;
    uint32_t request;
    uint32_t opcode;
    uint64_t address;
    uint32_t value;
    std::vector<uint8_t> segment;
};
struct Board {
    uart::FileDescriptor master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
    std::string port;
    uart_ipc_rx_t rx{};
    std::deque<Command> pending;
    std::vector<Command> history;
    Board()
    {
        CHECK(master.value >= 0); CHECK(::grantpt(master.value) == 0); CHECK(::unlockpt(master.value) == 0);
        port = ::ptsname(master.value); uart_ipc_rx_init(&rx);
    }
    static void receive(const uint8_t* bytes, size_t size, void* context)
    {
        auto& board = *static_cast<Board*>(context);
        std::array<uint8_t, UART_IPC_MAX_RAW> raw{};
        const auto decoded = cobs_decode(raw.data(), raw.size(), bytes, size);
        uart_ipc_hdr_t header{}; std::array<uint8_t, UART_IPC_MAX_PAYLOAD> payload{};
        CHECK(decoded.status == COBS_DECODE_OK);
        CHECK(uart_ipc_decode(raw.data(), decoded.out_len, &header, payload.data(), payload.size()) == UART_IPC_OK);
        CHECK(header.type == UART_IPC_TYPE_CMD);
        Command command{header.session_id, header.request_id, uint32_t(uart::get_le(payload.data(), 4)),
                        uart::get_le(payload.data() + 4, 8), uint32_t(uart::get_le(payload.data() + 12, 4)),
                        std::vector<uint8_t>(bytes, bytes + size)};
        board.history.push_back(command); board.pending.push_back(command);
    }
    Command next()
    {
        const auto deadline = uart::monotonic_ms() + 2000;
        while (pending.empty()) {
            CHECK(uart::wait_fd(master.value, POLLIN, deadline));
            std::array<uint8_t, 4096> bytes{};
            const auto n = ::read(master.value, bytes.data(), bytes.size());
            CHECK(n > 0); uart_ipc_rx_feed(&rx, bytes.data(), size_t(n), receive, this);
        }
        auto command = pending.front(); pending.pop_front(); return command;
    }
    void send(uint8_t type, uint64_t session, uint32_t request, uint32_t a, uint32_t b,
              const std::string& text = {})
    {
        uart_ipc_hdr_t header{1, type, uint16_t(8 + text.size()), session, request};
        std::vector<uint8_t> payload(8 + text.size());
        uart::put_le(payload.data(), a, 4); uart::put_le(payload.data() + 4, b, 4);
        std::copy(text.begin(), text.end(), payload.begin() + 8);
        std::array<uint8_t, UART_IPC_MAX_WIRE> bytes{}; size_t count = 0;
        CHECK(uart_ipc_encode(&header, payload.data(), bytes.data(), bytes.size(), &count) == UART_IPC_OK);
        size_t done = 0;
        while (done < count) {
            CHECK(uart::wait_fd(master.value, POLLOUT, uart::monotonic_ms() + 1000));
            // Deliberate fragmentation exercises the actual service stream RX.
            const auto n = ::write(master.value, bytes.data() + done, std::min<size_t>(3, count - done));
            CHECK(n > 0); done += size_t(n);
        }
    }
    void reply(const Command& command, uint32_t status = 0, uint32_t value = 0)
    {
        send(UART_IPC_TYPE_RSP, command.session, command.request, status, value);
    }
};
struct OutputCapture {
    uart::FileDescriptor saved{::dup(STDOUT_FILENO)};
    FILE* file = std::tmpfile();
    OutputCapture() { CHECK(file && saved.value >= 0); CHECK(::dup2(::fileno(file), STDOUT_FILENO) >= 0); }
    ~OutputCapture() { ::dup2(saved.value, STDOUT_FILENO); if (file) std::fclose(file); }
    std::string text()
    {
        std::fseek(file, 0, SEEK_SET);
        std::string text; std::array<char, 4096> bytes{};
        size_t n;
        while ((n = std::fread(bytes.data(), 1, bytes.size(), file)) != 0) text.append(bytes.data(), n);
        return text;
    }
};
struct Service {
    std::atomic_bool stop{false};
    std::exception_ptr error;
    std::thread worker;
    explicit Service(const uart::ConsoleConfig& config)
        : worker([&, config] { try { uart::run_console(config, stop); } catch (...) { error = std::current_exception(); } }) {}
    void finish() { stop.store(true); if (worker.joinable()) worker.join(); if (error) std::rethrow_exception(error); }
    ~Service() { stop.store(true); if (worker.joinable()) worker.join(); }
};
void wait_ready(const std::string& target)
{
    const auto deadline = uart::monotonic_ms() + 2000;
    while (uart::monotonic_ms() < deadline) {
        UartBridge probe;
        if (probe.bind(target)) return;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("console did not start");
}
void integration()
{
    Board board;
    uart::ConsoleConfig config;
    config.target = "uart-test-" + std::to_string(::getpid()); config.port = board.port;
    config.retry.retry_count = 1; config.retry.retry_interval_ms = 20; config.retry.response_timeout_ms = 150;
    OutputCapture output;
    uint64_t first_session = 0;
    {
        Service service(config); wait_ready(config.target);
        bool duplicate_rejected = false;
        try { uart::run_console(config, service.stop); } catch (const std::exception&) { duplicate_rejected = true; }
        CHECK(duplicate_rejected);
        auto other_target = config; other_target.target += "-other";
        duplicate_rejected = false;
        try { uart::run_console(other_target, service.stop); } catch (const std::exception&) { duplicate_rejected = true; }
        CHECK(duplicate_rejected);

        DeviceContext context; context.bdf = "uart-pty"; context.tpu_type = TPUType::Atlas;
        context.target_name = config.target;
        HalContext hal; hal.bind_device(context);
        {
            auto scope = hal.begin_testcase(nullptr, {CtxResource::uart()}); CHECK(scope);
            auto* ctx = hal.uart_context(); CHECK(ctx && ctx->env.base == 0);
            uint32_t value = 0;
            auto result = std::async(std::launch::async, [&] { return phal_read(ctx, 0x02105060, &value); });
            auto command = board.next(); first_session = command.session;
            CHECK(command.address == 0x02105060 && command.opcode == 0 && command.request == 1);
            const auto old_session = command.session == 1 ? 2 : command.session ^ 1u;
            board.send(UART_IPC_TYPE_LOG, old_session, 0, 1, 0, "old session PMU log\n");
            // Current-session LOGs are displayed even while waiting for an RSP.
            board.send(UART_IPC_TYPE_LOG, command.session, 0, 2, 0, "current session PMU log\n");
            board.send(UART_IPC_TYPE_RSP, old_session, command.request, 0, 0xbad);
            board.send(UART_IPC_TYPE_RSP, command.session, command.request + 10, 0, 0xbad);
            CHECK(result.wait_for(20ms) == std::future_status::timeout);
            board.reply(command, 0, 0x12345678);
            CHECK(result.get() == PHAL_STATUS_OK && value == 0x12345678);

            // Drop the first RSP; retry is byte-identical and keeps both IDs.
            result = std::async(std::launch::async, [&] { return phal_write(ctx, 0x02105060, 0xabcdef01); });
            command = board.next(); const auto retry = board.next();
            CHECK(command.opcode == 1 && command.value == 0xabcdef01);
            CHECK(retry.segment == command.segment); board.reply(retry);
            CHECK(result.get() == PHAL_STATUS_OK);
            for (const auto pair : {std::pair<uint32_t, phal_status_t>{1, PHAL_STATUS_INVALID},
                                   {2, PHAL_STATUS_TIMEOUT}, {4, PHAL_STATUS_ERROR}, {8, PHAL_STATUS_ERROR}}) {
                value = 0xfeed;
                result = std::async(std::launch::async, [&] { return phal_read(ctx, 0x02105060, &value); });
                command = board.next(); board.reply(command, pair.first, 0xbad);
                CHECK(result.get() == pair.second && value == 0xfeed);
            }
            result = std::async(std::launch::async, [&] { return phal_read(ctx, 0x02105060, &value); });
            command = board.next(); CHECK(board.next().segment == command.segment);
            CHECK(result.get() == PHAL_STATUS_TIMEOUT);
        }
        CHECK(hal.uart_context() == nullptr && hal.phal_context() == nullptr);
        // A new testcase/client keeps the console session and request sequence.
        AtlasResourceInfo resource; resource.valid = true; resource.platform = AtlasPlatform::Fpga;
        TPUDeviceConfig device_config; device_config.pmu_modules.push_back({0});
        TPUDevice device(config.target, context, device_config, nullptr, resource);
        hal.bind_device(device.get_context());
        auto* pmu = device.module("pmu", 0); CHECK(pmu);
        auto result = std::async(std::launch::async, [&] {
            return pmu->run_testcase("pmu_uart_read32_example", {{"addr", "0x02105060"}}, nullptr, &hal);
        });
        auto command = board.next(); CHECK(command.session == first_session && command.request > 1);
        board.reply(command, 0, 42); CHECK(result.get() == PHAL_STATUS_OK);
        CHECK(hal.uart_context() == nullptr);

        UartBridge a, b; CHECK(a.bind(config.target) && b.bind(config.target));
        uint32_t x = 0, y = 0;
        auto one = std::async(std::launch::async, [&] { return a.read(0x100, x); });
        auto two = std::async(std::launch::async, [&] { return b.read(0x104, y); });
        command = board.next();
        pollfd pending{board.master.value, POLLIN, 0};
        CHECK(::poll(&pending, 1, 20) == 0); // Second client waits for the first RSP.
        board.reply(command, 0, uint32_t(command.address));
        command = board.next(); board.reply(command, 0, uint32_t(command.address));
        CHECK(one.get() == UartResult::Ok && two.get() == UartResult::Ok && x == 0x100 && y == 0x104);
        service.finish();
        CHECK(a.read(0x100, x) == UartResult::Transport); // No reconnect/replay.
    }
    {
        Service service(config); wait_ready(config.target);
        UartBridge bridge; CHECK(bridge.bind(config.target));
        uint32_t value = 0;
        auto result = std::async(std::launch::async, [&] { return bridge.read(0x100, value); });
        const auto command = board.next(); CHECK(command.session != first_session && command.request == 1);
        board.reply(command, 0, 99); CHECK(result.get() == UartResult::Ok && value == 99);
        service.finish();
    }
    const auto captured = output.text();
    CHECK(captured.find("current session PMU log\n") != std::string::npos);
    CHECK(captured.find("old session PMU log") == std::string::npos);
}
} // namespace
int main()
{
    try { integration(); std::cout << "UART PTY/socket/PHAL integration passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "UART integration failed: " << e.what() << '\n'; return 1; }
}
