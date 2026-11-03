#include "uart/UartProtocol.h"
#include "uart/UartRpc.h"
#include "diag/core/HalContext.h"
#include "diag/device/TPUDevice.h"

extern "C" {
#include "cobs.h"
#include "crc32.h"
}

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

namespace {
std::vector<uint8_t> from_hex(const std::string& s)
{
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < s.size(); i += 2) bytes.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
    return bytes;
}
// Independent payload construction, including deliberately invalid request IDs
// that the firmware encoder would refuse to generate.
std::vector<uint8_t> frame(uint8_t type, uint64_t session, uint32_t request,
                           uint32_t status, uint32_t value, const std::string& text = {})
{
    std::vector<uint8_t> raw(28 + text.size());
    raw[0] = 1; raw[1] = type;
    uart::put_le(raw.data() + 2, 8 + text.size(), 2);
    uart::put_le(raw.data() + 4, session, 8); uart::put_le(raw.data() + 12, request, 4);
    uart::put_le(raw.data() + 16, status, 4); uart::put_le(raw.data() + 20, value, 4);
    std::copy(text.begin(), text.end(), raw.begin() + 24);
    uart::put_le(raw.data() + raw.size() - 4, crc32_ieee(raw.data(), raw.size() - 4), 4);
    std::vector<uint8_t> wire(UART_IPC_MAX_WIRE);
    const auto encoded = cobs_encode(wire.data() + 1, wire.size() - 2, raw.data(), raw.size());
    CHECK(encoded.status == COBS_ENCODE_OK);
    wire.resize(encoded.out_len + 2); wire.front() = 0; wire.back() = 0;
    return wire;
}
void golden_vectors_and_streams()
{
    constexpr uint64_t SESSION = UART_IPC_SESSION_V1;
    // Fixed independent vectors supplied with the firmware/Python reference.
    CHECK(uart::encode_command(SESSION, 7, true, 0x1122334455667788ull, 0xa5a5a5a5) == from_hex(
        "000401011002010101020101010207010102010101118877665544332211a5a5a5a52c05fbff00"));
    CHECK(uart::encode_command(SESSION, 7, false, 0x10000007, 0) == from_hex(
        "000401011002010101020101010207010101010101020701021001010101010101058311572f00"));
    const auto rsp = from_hex("000401020802010101020101010207010101010101097856341278418f4700");
    CHECK(frame(UART_IPC_TYPE_RSP, SESSION, 7, 0, 0x12345678) == rsp);
    CHECK(frame(UART_IPC_TYPE_LOG, SESSION, 0, 0x11223344, 0x55667788) == from_hex(
        "00040103080201010102010101010101010b443322118877665561fa025400"));
    const std::string crc_input = "123456789";
    CHECK(crc32_ieee(reinterpret_cast<const uint8_t*>(crc_input.data()), crc_input.size()) == 0xcbf43926);
    std::vector<uart::Frame> received;
    std::string boot;
    uart::StreamDecoder decoder(SESSION, [&](const auto& f) { received.push_back(f); },
                                [&](const auto& s) { boot += s; });
    for (const auto byte : rsp) decoder.feed(&byte, 1);
    auto log = frame(UART_IPC_TYPE_LOG, SESSION, 0, 10, 3, "current session log\n");
    std::vector<uint8_t> mixed(log); mixed.insert(mixed.end(), rsp.begin(), rsp.end());
    decoder.feed(mixed.data(), mixed.size());
    CHECK(received.size() == 3);
    CHECK(received[0].value == 0x12345678 && received[0].request == 7);
    CHECK(received[1].session == SESSION && received[1].text == "current session log\n");
    CHECK(received[1].log_sequence == 10 && received[1].dropped_total == 3);
    const auto stale_log = frame(UART_IPC_TYPE_LOG, 999, 0, 11, 0, "old session log\n");
    const auto stale_rsp = frame(UART_IPC_TYPE_RSP, 999, 7, 0, 0xbad);
    decoder.feed(stale_log.data(), stale_log.size());
    decoder.feed(stale_rsp.data(), stale_rsp.size());
    CHECK(decoder.stale_session_frames() == 2 && received.size() == 3 && boot.empty());
    auto bad = rsp; bad[bad.size() - 3] ^= 0x40;
    decoder.feed(bad.data(), bad.size());
    auto invalid = frame(UART_IPC_TYPE_RSP, SESSION, 0, 0, 0);
    decoder.feed(invalid.data(), invalid.size());
    invalid = frame(UART_IPC_TYPE_LOG, SESSION, 7, 0, 0);
    decoder.feed(invalid.data(), invalid.size());
    CHECK(decoder.bad_frames() == 3 && received.size() == 3);
    std::vector<uint8_t> oversized(1000, 0x55); oversized.push_back(0);
    decoder.feed(oversized.data(), oversized.size());
    CHECK(decoder.oversized_frames() == 1);
    decoder.feed(rsp.data(), 10); decoder.flush_partial_text();
    decoder.feed(rsp.data() + 10, rsp.size() - 10); CHECK(received.size() == 4);
    const std::string text = "PMU boot\r\n";
    decoder.feed(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    CHECK(boot == text);
}
void serial_retry_contract()
{
    uart::RetryConfig config; config.retry_count = 2; config.response_timeout_ms = 100; config.retry_interval_ms = 20;
    uart::Transaction tx(77, config);
    CHECK(tx.start(true, 0x1122334455667788ull, 0x12345678, 0));
    const auto original = tx.wire();
    CHECK(!tx.start(false, 0x100, 0, 0)); // No interleaving, including retry.
    tx.sent(0); tx.tick(99); CHECK(tx.active() && !tx.needs_send());
    uart::Frame response; response.type = UART_IPC_TYPE_RSP; response.session = 78; response.request = 1;
    tx.receive(response); CHECK(tx.active());
    response.session = 77; response.request = 2; tx.receive(response); CHECK(tx.active());
    response.type = UART_IPC_TYPE_LOG; response.request = 1; tx.receive(response); CHECK(tx.active());
    tx.tick(100); CHECK(!tx.needs_send());
    tx.tick(119); CHECK(!tx.needs_send());
    tx.tick(120); CHECK(tx.needs_send() && tx.wire() == original);
    tx.sent(120); tx.tick(220); tx.tick(240);
    CHECK(tx.needs_send() && tx.wire() == original && tx.request_id() == 1);
    tx.sent(240); tx.tick(340); CHECK(!tx.active() && tx.result().result == UartResult::Timeout);
    CHECK(tx.start(false, 0x100, 0, 350) && tx.request_id() == 2);
    tx.sent(350); tx.tick(450); // A late response during backoff completes it.
    response.type = UART_IPC_TYPE_RSP; response.request = 2; response.value = 0xbeef;
    tx.receive(response); tx.tick(500);
    CHECK(!tx.active() && tx.result().result == UartResult::Ok && tx.result().value == 0xbeef);
    // Duplicate and old responses cannot complete the next logical request.
    CHECK(tx.start(false, 0x100, 0, 600)); tx.sent(600); tx.receive(response); CHECK(tx.active());
    for (const uint32_t status : {1u, 2u, 4u, 8u, 0x80u}) {
        response.request = tx.request_id(); response.status = status;
        tx.receive(response); tx.tick(10000);
        CHECK(!tx.active() && tx.result().result == UartResult::Firmware && tx.result().firmware_status == status);
        CHECK(tx.start(false, 0x100, 0, 0)); tx.sent(0);
    }
    tx.fail(UartResult::Transport); CHECK(!tx.active());
    uart::Transaction exhausted(77, config, std::numeric_limits<uint32_t>::max());
    CHECK(!exhausted.start(false, 0x100, 0, 0));
    CHECK(exhausted.result().result == UartResult::Transport);
    uart::Transaction invalid(77, config);
    CHECK(!invalid.start(false, 3, 0, 0) && invalid.request_id() == 0);
    config.retry_count = 0;
    uart::Transaction once(77, config); CHECK(once.start(false, 0, 0, 0)); once.sent(0); once.tick(100);
    CHECK(!once.active() && once.result().result == UartResult::Timeout);
    for (auto bad : {uart::RetryConfig{0, 0, 0}, uart::RetryConfig{0xffffffffu, 0xffffffffu, 0xffffffffu}}) {
        bool rejected = false; try { bad.validate(); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
}
void local_rpc_contract()
{
    uart::RpcMessage sent; sent.kind = uart::RpcKind::Write; sent.id = 123;
    sent.address = 0x1122334455667788ull; sent.value = 0xaabbccdd;
    auto bytes = uart::encode_rpc(sent);
    uart::RpcMessage received; CHECK(uart::decode_rpc(bytes.data(), received));
    CHECK(received.kind == sent.kind && received.id == 123 && received.address == sent.address && received.value == sent.value);
    bytes[36] = 1; CHECK(!uart::decode_rpc(bytes.data(), received));
    bool rejected = false;
    try { uart::socket_path("../TPU0"); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
class UartProbe : public TestTarget {
public:
    explicit UartProbe(const DeviceContext& ctx) : TestTarget("probe", "probe", ctx)
    {
        _add_test("uart", {}, [](TestInfo&) -> TestStatus {
            throw std::runtime_error("non-FPGA UART testcase must never execute");
        }, {CtxResource::uart()});
    }
};
void fpga_gate()
{
    for (auto platform : {AtlasPlatform::Chip, AtlasPlatform::Emu, AtlasPlatform::Unknown}) {
        DeviceContext context; context.bdf = "uart-platform-test";
        HalContext hal; hal.bind_device(context);
        AtlasResourceInfo resource; resource.valid = true; resource.platform = platform;
        TPUDeviceConfig config;
        TPUDevice device("TPU0", context, config, nullptr, resource);
        UartProbe probe(context); probe.set_device(&device);
        CHECK(probe.run_testcase("probe_uart", {}, nullptr, &hal) == PHAL_STATUS_UNIMPLEMENTED);
        CHECK(hal.uart_context() == nullptr && hal.phal_context() == nullptr);
    }
}
} // namespace

int main()
{
    try {
        golden_vectors_and_streams(); serial_retry_contract(); local_rpc_contract(); fpga_gate();
        std::cout << "UART protocol, retry, RPC and FPGA contracts passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "UART test failed: " << e.what() << '\n'; return 1;
    }
}
