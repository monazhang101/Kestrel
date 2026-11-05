#include "UartProtocol.h"
#include "UartWire.h"

extern "C" {
#include "cobs.h"
}

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace uart {

std::vector<uint8_t> encode_command(uint64_t session, uint32_t request,
                                    bool write, uint64_t address, uint32_t value)
{
    uart_ipc_hdr_t header{UART_IPC_VERSION, UART_IPC_TYPE_CMD, 16, session, request};
    std::array<uint8_t, 16> payload{};
    put_le(payload.data(), write ? 1 : 0, 4);
    put_le(payload.data() + 4, address, 8);
    put_le(payload.data() + 12, write ? value : 0, 4);
    std::vector<uint8_t> wire(UART_IPC_MAX_WIRE);
    size_t size = 0;
    if (uart_ipc_encode(&header, payload.data(), wire.data(), wire.size(), &size) != UART_IPC_OK)
        throw std::invalid_argument("invalid UART command");
    wire.resize(size);
    return wire;
}

StreamDecoder::StreamDecoder(uint64_t session, FrameHandler frames, TextHandler text)
    : session_(session), frames_(std::move(frames)), text_(std::move(text))
{
    if (!session) throw std::invalid_argument("UART receive session must be nonzero");
    uart_ipc_rx_init(&rx_);
}

bool StreamDecoder::printable(const uint8_t* bytes, size_t count) const
{
    if (count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        const auto c = bytes[i];
        if (!(c >= 32 && c < 127) && c != '\r' && c != '\n') return false;
    }
    return true;
}
void StreamDecoder::receive(const uint8_t* bytes, size_t count, void* context)
{
    static_cast<StreamDecoder*>(context)->decode(bytes, count);
}
void StreamDecoder::decode(const uint8_t* bytes, size_t count)
{
    std::array<uint8_t, UART_IPC_MAX_RAW> raw{};
    const auto decoded = cobs_decode(raw.data(), raw.size(), bytes, count);
    uart_ipc_hdr_t h{};
    std::array<uint8_t, UART_IPC_MAX_PAYLOAD> payload{};
    if (decoded.status != COBS_DECODE_OK ||
        uart_ipc_decode(raw.data(), decoded.out_len, &h, payload.data(), payload.size()) != UART_IPC_OK ||
        (h.type != UART_IPC_TYPE_LOG && h.type != UART_IPC_TYPE_RSP) ||
        (h.type == UART_IPC_TYPE_LOG ? h.request_id != 0 : h.request_id == 0)) {
        ++rx_.dropped_bad_frame;
        if (text_ && printable(bytes, count)) text_(std::string(reinterpret_cast<const char*>(bytes), count));
        return;
    }
    // Match Link._feed in the Python reference: both LOG and RSP from
    // another session are counted and excluded from the received frames.
    if (h.session_id != session_) { ++stale_session_frames_; return; }
    Frame f;
    f.type = h.type; f.session = h.session_id; f.request = h.request_id;
    if (h.type == UART_IPC_TYPE_RSP) {
        f.status = uint32_t(get_le(payload.data(), 4));
        f.value = uint32_t(get_le(payload.data() + 4, 4));
    } else {
        f.log_sequence = uint32_t(get_le(payload.data(), 4));
        f.dropped_total = uint32_t(get_le(payload.data() + 4, 4));
        f.text.assign(reinterpret_cast<const char*>(payload.data() + 8), h.payload_len - 8);
    }
    if (frames_) frames_(f);
}
void StreamDecoder::feed(const uint8_t* bytes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uart_ipc_rx_feed(&rx_, bytes + i, 1, receive, this);
        // Boot prints have no frame delimiter. Valid V1 binary headers contain
        // non-printable bytes, so a wholly printable line is boot text.
        if (bytes[i] == '\n' && text_ && printable(rx_.buf, rx_.len)) {
            text_(std::string(reinterpret_cast<const char*>(rx_.buf), rx_.len));
            uart_ipc_rx_idle_reset(&rx_);
        }
    }
}
void StreamDecoder::flush_partial_text()
{
    if (rx_.len && text_ && printable(rx_.buf, rx_.len)) {
        text_(std::string(reinterpret_cast<const char*>(rx_.buf), rx_.len));
        uart_ipc_rx_idle_reset(&rx_);
    }
}

void RetryConfig::validate() const
{
    constexpr uint64_t LIMIT = std::numeric_limits<uint32_t>::max();
    const uint64_t initial = uint64_t(response_timeout_ms) + 1000;
    const uint64_t retry = initial + retry_interval_ms;
    if (response_timeout_ms == 0 || initial > LIMIT ||
        uint64_t(retry_count) > (LIMIT - initial) / retry)
        throw std::invalid_argument("invalid UART retry/timeout configuration");
}
Transaction::Transaction(uint64_t session, RetryConfig config, uint32_t last_request)
    : session_(session), config_(config), request_(last_request)
{
    config_.validate();
    if (!session) throw std::invalid_argument("UART session must be nonzero");
}
bool Transaction::active() const
{
    return state_ == State::Sending || state_ == State::Waiting || state_ == State::Backoff;
}
bool Transaction::needs_send() const { return state_ == State::Sending; }
bool Transaction::start(bool write, uint64_t address, uint32_t value, uint64_t now)
{
    if (active()) return false;
    result_ = {};
    if (address % 4 != 0) { result_.result = UartResult::Invalid; state_ = State::Done; return false; }
    if (request_ == std::numeric_limits<uint32_t>::max()) {
        result_.result = UartResult::Transport; state_ = State::Done; return false;
    }
    wire_ = encode_command(session_, ++request_, write, address, value);
    retries_ = 0; deadline_ = now; state_ = State::Sending;
    return true;
}
void Transaction::sent(uint64_t now)
{
    if (!needs_send()) return;
    state_ = State::Waiting; deadline_ = now + config_.response_timeout_ms;
}
void Transaction::tick(uint64_t now)
{
    if (state_ == State::Waiting && now >= deadline_) {
        if (retries_ == config_.retry_count) { fail(UartResult::Timeout); return; }
        ++retries_; state_ = State::Backoff; deadline_ = now + config_.retry_interval_ms;
    }
    if (state_ == State::Backoff && now >= deadline_) state_ = State::Sending;
}
void Transaction::receive(const Frame& f)
{
    if (!active() || f.type != UART_IPC_TYPE_RSP || f.session != session_ || f.request != request_) return;
    result_ = {f.status == UART_IPC_OK ? UartResult::Ok : UartResult::Firmware, f.status, f.value};
    state_ = State::Done;
}
void Transaction::fail(UartResult result) { result_ = {result, 0, 0}; state_ = State::Done; }

} // namespace uart
