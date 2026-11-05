#pragma once

#include "diag/core/UartBridge.h"

extern "C" {
#include "uart_ipc_proto.h"
#include "uart_ipc_rx.h"
}

#include <functional>
#include <string>
#include <vector>

namespace uart {

struct Frame {
    uint8_t type = 0;
    uint64_t session = 0;
    uint32_t request = 0;
    uint32_t status = 0;
    uint32_t value = 0;
    uint32_t log_sequence = 0;
    uint32_t dropped_total = 0;
    std::string text;
};

std::vector<uint8_t> encode_command(uint64_t session, uint32_t request,
                                    bool write, uint64_t address, uint32_t value);

class StreamDecoder {
public:
    using FrameHandler = std::function<void(const Frame&)>;
    using TextHandler = std::function<void(const std::string&)>;
    StreamDecoder(uint64_t session, FrameHandler frames, TextHandler text = {});
    void feed(const uint8_t* bytes, size_t count);
    // Like Link._dump_junk: only printable text can be flushed; an
    // incomplete binary frame remains available for the next serial read.
    void flush_partial_text();
    uint32_t bad_frames() const { return rx_.dropped_bad_frame; }
    uint32_t oversized_frames() const { return rx_.dropped_oversize; }
    uint64_t stale_session_frames() const { return stale_session_frames_; }

private:
    static void receive(const uint8_t* bytes, size_t count, void* context);
    void decode(const uint8_t* bytes, size_t count);
    bool printable(const uint8_t* bytes, size_t count) const;
    uart_ipc_rx_t rx_{};
    uint64_t session_;
    uint64_t stale_session_frames_ = 0;
    FrameHandler frames_;
    TextHandler text_;
};

struct TransactionResult {
    UartResult result = UartResult::Ok;
    uint32_t firmware_status = 0;
    uint32_t value = 0;
};

// One logical transaction per port. The transport owns this across testcase
// context lifetimes. Time is injected as monotonic milliseconds for deterministic tests.
class Transaction {
public:
    explicit Transaction(uint64_t session, RetryConfig config = {}, uint32_t last_request = 0);
    bool start(bool write, uint64_t address, uint32_t value, uint64_t now);
    bool active() const;
    bool needs_send() const;
    const std::vector<uint8_t>& wire() const { return wire_; }
    void sent(uint64_t now);
    void tick(uint64_t now);
    void receive(const Frame& frame);
    void fail(UartResult result);
    const TransactionResult& result() const { return result_; }
    uint32_t request_id() const { return request_; }

private:
    enum class State { Idle, Sending, Waiting, Backoff, Done };
    State state_ = State::Idle;
    uint64_t session_;
    RetryConfig config_;
    uint32_t request_;
    uint32_t retries_ = 0;
    uint64_t deadline_ = 0;
    std::vector<uint8_t> wire_;
    TransactionResult result_;
};

} // namespace uart
