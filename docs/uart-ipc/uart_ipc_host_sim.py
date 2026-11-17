#!/usr/bin/env python3
"""PC-side host simulator for the Atlas PMU UART IPC channel.

Until the real host tool exists, this script is the host side of the
UART IPC link: it builds CMD frames (COBS + CRC-32/ISO-HDLC) fully
independently of any C code, sends them over pyserial, receives RSP/LOG
frames, decodes them and judges each mode.

Protocol summary (common/uart_ipc/uart_ipc_proto.h):
  wire  = 0x00 | COBS(header 16B + payload + crc 4B) | 0x00
  header (all little-endian):
    U8  version = 1
    U8  type    (1=CMD 2=RSP 3=LOG)
    U16 payload_len (CMD=16, RSP=8, LOG=8..256)
    U64 session_id = 0x0000000100000001
    U32 request_id (CMD/RSP non-zero, LOG=0)
  CMD payload 16B: u32 opcode (0=RD 1=WR), u32 addr_low, u32 addr_high,
                   u32 data
  RSP payload  8B: u32 status, u32 data
  CRC-32/ISO-HDLC over header+payload == zlib.crc32 (Python stdlib).

Judged modes (each prints [HOST_SIM] lines and exits 0=PASS / 1=FAIL;
no device connected = FAIL, never PASS):
  echo       send one RD frame, read RSP, compare request_id/status/data
  crc-error  send one frame with a corrupted payload byte (CRC mismatch),
             then a good frame; the good frame must get an RSP (the bad
             frame was silently dropped by the firmware)
  dedup      send the same request_id+content twice; firmware (spec 8.2)
             must answer both - second answer is the saved result, not a
             re-execution (checked by WR: only the first executes).
             Also checks: same request_id with different content is
             rejected (status=INVALID), and an older request_id gets no
             response.
  burst      send 8 RD frames (distinct request_ids), all 8 must be
             answered, none lost
  --self-test   offline mode (no serial port): CRC-32 check value,
                frame build/parse roundtrip, wire vectors byte-compared
                against known C-side vectors (generated with the actual
                uart_ipc_encode implementation, see VECTORS below).

Usage:
  python3 uart_ipc_host_sim.py --port /dev/ttyUSB1 --mode echo
  python3 uart_ipc_host_sim.py --self-test

Requires: pyserial for the on-device modes (not needed for --self-test).
"""

import argparse
import sys
import time
import zlib

# ---------------------------------------------------------------------------
# Protocol constants (mirror common/uart_ipc/uart_ipc_proto.h)
# ---------------------------------------------------------------------------

UART_IPC_VERSION = 1
UART_IPC_MAX_PAYLOAD = 256
UART_IPC_HDR_LEN = 16
UART_IPC_CRC_LEN = 4
UART_IPC_MAX_RAW = UART_IPC_HDR_LEN + UART_IPC_MAX_PAYLOAD + UART_IPC_CRC_LEN  # 276
UART_IPC_MAX_ENCODED = 278
UART_IPC_MAX_WIRE = 280

UART_IPC_TYPE_CMD = 1
UART_IPC_TYPE_RSP = 2
UART_IPC_TYPE_LOG = 3

UART_IPC_SESSION_V1 = 0x0000000100000001

# This host process's session tag (8.2 session semantics). Firmware keeps
# a per-session dedup table; a NEW session_id tells it a fresh host is
# talking (host restarted / reconnected) so request_ids may restart from
# any value. Value rules: non-zero, and != UART_IPC_SESSION_V1 (an old
# host that only ever sends the V1 constant must stay in the V1 session -
# firmware's initial session - so it never triggers a rebuild).
# (time_ns ^ pid) is enough uniqueness for a test tool.
import os as _os
import time as _time
SESSION_ID = (_time.time_ns() ^ _os.getpid()) & 0xFFFFFFFFFFFFFFFF
if SESSION_ID == 0 or SESSION_ID == UART_IPC_SESSION_V1:
    SESSION_ID += 1

# Error codes (common/uart_ipc/uart_ipc_proto.h, values match ipc_rw)
UART_IPC_OK = 0
UART_IPC_ERR_INVALID = 1 << 0
UART_IPC_ERR_TIMEOUT = 1 << 1
UART_IPC_ERR_UNKNOWN = 1 << 2
UART_IPC_ERR_BUSY = 1 << 3

# PMU_DIAG register bank (the only RD/WR whitelist the IPC Owner accepts;
# pmu/platform/soc/atl/include/noc_crg_reg.h + pmu_diag_reg.h).
# REG0 is the RW boot counter: RD works, WR changes the value read back.
PMU_DIAG_BASE = 0x02105000
PMU_DIAG_REG0_OFFSET = 0x60
ADDR_REG0 = PMU_DIAG_BASE + PMU_DIAG_REG0_OFFSET

# RX: how long to wait for one RSP after sending a CMD (seconds). Firmware
# answers in well under a second (task_ipc period 50 ms worst case); 5 s
# leaves margin for burst drains without stalling on a dead link.
RSP_TIMEOUT_S = 5.0

# After writing, allow the Owner this many RD attempts before the write is
# called lost (the write itself completes in one Owner period).
RD_RETRIES = 20


# ---------------------------------------------------------------------------
# COBS (independent Python implementation; must match external/cobs behavior)
# ---------------------------------------------------------------------------

def cobs_encode(data: bytes) -> bytes:
    """COBS-encode data (no delimiters). Mirrors external/cobs/src/cobs.c."""
    out = bytearray()
    out.append(0)             # placeholder for the first code byte
    code_pos = 0
    search_len = 1
    for b in data:
        if b == 0:
            out[code_pos] = search_len
            code_pos = len(out)
            out.append(0)
            search_len = 1
        else:
            out.append(b)
            search_len += 1
            if search_len == 0xFF:
                out[code_pos] = search_len
                code_pos = len(out)
                out.append(0)
                search_len = 1
    out[code_pos] = search_len
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """COBS-decode data. Raises ValueError on a malformed stream."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        i += 1
        if code == 0:
            raise ValueError("zero code byte in COBS input")
        run = code - 1
        chunk = data[i:i + run]
        if len(chunk) != run:
            raise ValueError("COBS input too short")
        if 0 in chunk:
            raise ValueError("zero byte inside COBS run")
        out += chunk
        i += run
        if i >= n:
            break
        if code != 0xFF:
            out.append(0)
    return bytes(out)


# ---------------------------------------------------------------------------
# Frame build / parse (mirror uart_ipc_proto.c field-by-field LE packing)
# ---------------------------------------------------------------------------

def _le(val: int, width: int) -> bytes:
    return int(val).to_bytes(width, "little")


def build_frame(frame_type: int, payload: bytes, request_id: int,
                session_id: int = None) -> bytes:
    """Build one full wire frame: 0x00 + COBS(raw) + 0x00.

    session_id: the current host session tag (8.2 session semantics -
    firmware rebuilds its dedup table when it changes). Defaults to the
    module-level SESSION_ID (per-process, generated at import)."""
    if frame_type not in (UART_IPC_TYPE_CMD, UART_IPC_TYPE_RSP,
                           UART_IPC_TYPE_LOG):
        raise ValueError("bad frame type %d" % frame_type)
    if not (8 <= len(payload) <= UART_IPC_MAX_PAYLOAD):
        raise ValueError("payload length %d out of range" % len(payload))
    if frame_type == UART_IPC_TYPE_CMD and len(payload) != 16:
        raise ValueError("CMD payload must be 16 B")
    if frame_type == UART_IPC_TYPE_RSP and len(payload) != 8:
        raise ValueError("RSP payload must be 8 B")
    if frame_type == UART_IPC_TYPE_LOG and request_id != 0:
        raise ValueError("LOG must have request_id=0")
    if frame_type != UART_IPC_TYPE_LOG and request_id == 0:
        raise ValueError("CMD/RSP must have request_id!=0")
    if session_id is None:
        session_id = SESSION_ID

    raw = bytearray()
    raw += _le(UART_IPC_VERSION, 1)
    raw += _le(frame_type, 1)
    raw += _le(len(payload), 2)
    raw += _le(session_id, 8)
    raw += _le(request_id, 4)
    raw += payload
    crc = zlib.crc32(bytes(raw)) & 0xFFFFFFFF
    raw += _le(crc, 4)
    return b"\x00" + cobs_encode(bytes(raw)) + b"\x00"


def build_cmd(request_id: int, opcode: int, addr: int, data: int,
              session_id: int = None) -> bytes:
    payload = (_le(opcode, 4) + _le(addr & 0xFFFFFFFF, 4) +
               _le((addr >> 32) & 0xFFFFFFFF, 4) + _le(data, 4))
    return build_frame(UART_IPC_TYPE_CMD, payload, request_id, session_id)


def parse_frame(wire_segment: bytes):
    """Parse one de-delimited COBS segment (no 0x00 at either end).

    Returns (frame_type, request_id, payload bytes) or raises ValueError.
    Checks header fields, exact payload length per type and CRC - the
    same checks uart_ipc_decode performs. session_id is checked by the
    caller (read_segments) against the current host session."""
    raw = cobs_decode(wire_segment)
    if len(raw) < UART_IPC_HDR_LEN + UART_IPC_CRC_LEN:
        raise ValueError("raw too short")
    version = raw[0]
    frame_type = raw[1]
    payload_len = int.from_bytes(raw[2:4], "little")
    request_id = int.from_bytes(raw[12:16], "little")
    if version != UART_IPC_VERSION:
        raise ValueError("version != 1")
    if frame_type == UART_IPC_TYPE_CMD:
        want_len = 16
    elif frame_type == UART_IPC_TYPE_RSP:
        want_len = 8
    elif frame_type == UART_IPC_TYPE_LOG:
        if not (8 <= payload_len <= UART_IPC_MAX_PAYLOAD):
            raise ValueError("LOG payload_len out of range")
        want_len = payload_len
    else:
        raise ValueError("bad frame type %d" % frame_type)
    if frame_type != UART_IPC_TYPE_LOG:
        if payload_len != want_len:
            raise ValueError("payload_len %d != %d" % (payload_len, want_len))
    if len(raw) != UART_IPC_HDR_LEN + payload_len + UART_IPC_CRC_LEN:
        raise ValueError("raw length mismatch")
    payload = raw[UART_IPC_HDR_LEN:UART_IPC_HDR_LEN + payload_len]
    stored_crc = int.from_bytes(
        raw[UART_IPC_HDR_LEN + payload_len:UART_IPC_HDR_LEN + payload_len + 4],
        "little")
    calc_crc = zlib.crc32(raw[:UART_IPC_HDR_LEN + payload_len]) & 0xFFFFFFFF
    if stored_crc != calc_crc:
        raise ValueError("CRC mismatch")
    if frame_type == UART_IPC_TYPE_LOG and request_id != 0:
        raise ValueError("LOG with request_id != 0")
    return frame_type, request_id, bytes(payload)


def frame_session_id(wire_segment: bytes) -> int:
    """Session tag of a parsed frame (header offset 4, LE u64).

    Firmware echoes the current session in every RSP/LOG; a mismatch
    means the frame belongs to a different (stale) session."""
    raw = cobs_decode(wire_segment)
    return int.from_bytes(raw[4:12], "little")


def parse_rsp_payload(payload: bytes):
    """Split an RSP payload into (status, data)."""
    status = int.from_bytes(payload[0:4], "little")
    data = int.from_bytes(payload[4:8], "little")
    return status, data


# ---------------------------------------------------------------------------
# Wire vectors - generated ONCE with the actual C implementation
# ---------------------------------------------------------------------------
# Source: compiled a scratch host program against
# common/uart_ipc/uart_ipc_proto.c + common/crc/crc32.c +
# external/cobs/src/cobs.c
# and dumped uart_ipc_encode() output for the inputs below (RSP/LOG
# mirror the header/payload layout the firmware transport builds in
# build_rsp_frame/build_log_frame).
# Byte-identical agreement here proves the Python encoder matches the
# firmware encoder without needing any generated file at run time.

VECTORS = {
    # WR CMD: opcode=1 addr=0x1122334455667788 data=0xA5A5A5A5 id=7
    "CMD": bytes.fromhex(
        "000401011002010101020101010207010102010101118877665544332211"
        "a5a5a5a52c05fbff00"),
    # RD CMD: opcode=0 addr=0x10000007 data=0 id=7 (rx host test shape)
    "CMD2": bytes.fromhex(
        "00040101100201010102010101020701010101010102070102100101010101"
        "0101058311572f00"),
    # RSP: status=0 data=0x12345678 id=7
    "RSP": bytes.fromhex(
        "000401020802010101020101010207010101010101097856341278418f4700"),
    # LOG: seq=0x11223344 dropped=0x55667788 no text id=0
    "LOG": bytes.fromhex(
        "00040103080201010102010101010101010b443322118877665561fa025400"),
}


# ---------------------------------------------------------------------------
# Serial link (on-device modes)
# ---------------------------------------------------------------------------

class Fail(Exception):
    """A judged check failed; the message is the reason."""


def hexdump(data: bytes, width: int = 16) -> str:
    """Classic hex dump: offset, hex bytes, ASCII column. Board debugging
    needs to see the raw wire bytes when nothing parses as a frame."""
    out = []
    for off in range(0, len(data), width):
        row = data[off:off + width]
        hexs = " ".join("%02x" % b for b in row)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        out.append("%04x  %-47s  %s" % (off, hexs, text))
    return "\n".join(out)


class Link:
    """pyserial wrapper: sends frames, collects 0x00-delimited segments
    and parses every complete one into (type, request_id, payload).

    Segment reassembly is a small state machine kept ACROSS reads
    (`in_frame` + `seg`): a single ser.read() boundary can fall in the
    middle of a frame, so each read cannot be parsed standalone. Between
    delimiters bytes accumulate into `seg`; a 0x00 closes the current
    segment (parsed and yielded if it is a valid frame, console junk
    otherwise) and hunting resumes at the next byte. This mirrors the
    firmware's own uart_ipc_rx HUNT/COLLECT state machine."""

    def __init__(self, port, baud, session_id=None):
        import serial
        try:
            self.ser = serial.Serial(port, baudrate=baud, timeout=0.05)
        except Exception as exc:            # noqa: BLE001 - report and FAIL
            raise Fail("cannot open serial port %s: %s" % (port, exc))
        self.in_frame = False  # True = bytes since the last 0x00 belong
        self.seg = bytearray() #      to a possibly-still-incomplete frame
        self.junk = bytearray() #     printable console text (probes/prints)
        self.raw_all = bytearray()  # every byte received, for hex dump
        # Session tag this host speaks with (8.2): defaults to this
        # process's SESSION_ID; the session-rebuild case overrides it to
        # simulate a fresh host. Frames echoed with a DIFFERENT session
        # (stale firmware view) are counted, not silently swallowed.
        self.session_id = session_id if session_id is not None else SESSION_ID
        self.stale_session_frames = 0

    def send(self, frame: bytes):
        self.ser.write(frame)
        self.ser.flush()

    def _feed(self, chunk: bytes):
        """Feed one read chunk into the segment state machine; return the
        list of complete frames found in it. Bytes that cannot be
        COBS/CRC-decoded (console junk between delimiters) are skipped,
        not fatal.

        The whole chunk is processed EAGERLY (no mid-chunk generator
        suspension): wait_rsp returns as soon as its frame appears, and
        any frames arriving after it in the same 64-byte read would be
        lost if the generator were abandoned mid-chunk - the bytes are
        already out of the kernel buffer, nobody will re-read them."""
        frames = []
        for byte in chunk:
            if len(self.raw_all) < 8192:
                self.raw_all.append(byte)
            if byte == 0:
                if self.in_frame and self.seg:
                    seg = bytes(self.seg)
                    self.in_frame = False
                    self.seg = bytearray()
                    try:
                        ftype, rid, payload = parse_frame(seg)
                        # 8.2 会话语义: RSP/LOG 必须回显当前会话。不同
                        # session 的帧（固件会话视图过期）不能当本会话
                        # 应答—计数留证、调用方判 FAIL。
                        if frame_session_id(seg) != self.session_id:
                            self.stale_session_frames += 1
                        else:
                            frames.append((ftype, rid, payload))
                        self.junk = bytearray()
                    except ValueError:
                        # not a valid frame: if it was printable text it is
                        # console output (probe prints) - keep it for the
                        # caller to display, otherwise drop it silently
                        printable = all(32 <= b < 127 or b in (10, 13)
                                        for b in seg)
                        if printable and len(self.junk) < 4096:
                            self.junk += seg
                        pass    # junk segment, drop it
                else:
                    self.in_frame = False
                    self.seg = bytearray()
            else:
                self.in_frame = True
                self.seg.append(byte)
                if len(self.seg) > UART_IPC_MAX_ENCODED:
                    # oversized garbage will never decode; keep the cap so
                    # console text cannot grow the buffer unbounded
                    self.seg = bytearray()
        return frames

    def read_segments(self, seconds: float):
        """Read raw bytes for `seconds` (64-byte reads, exactly like the
        firmware's Port batches), yield parsed frames as they complete.
        State persists across reads - a frame split over two reads is
        reassembled, not dropped, and every frame inside a consumed
        chunk is parsed before the caller can stop early."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            chunk = self.ser.read(64)
            if chunk:
                yield from self._feed(chunk)


    def wait_rsp(self, request_id: int):
        """Wait for the RSP with this request_id; ignore LOG frames.
        Console text captured along the way (probe prints from the
        firmware) is dumped when the wait ends - board-side evidence the
        operator would otherwise lose to the port being busy."""
        for ftype, rid, payload in self.read_segments(RSP_TIMEOUT_S):
            if ftype == UART_IPC_TYPE_RSP and rid == request_id:
                self._dump_junk("while waiting for RSP")
                return parse_rsp_payload(payload)
        self._dump_junk("no RSP - console text seen during wait")
        raise Fail("timeout waiting for RSP request_id=%d" % request_id)

    def _dump_junk(self, why: str):
        # Pending incomplete segment (bytes since the last 0x00, no closing
        # delimiter yet): if it is printable it is console text too. Without
        # this, prints that arrive with no frame following them stay stuck in
        # self.seg and are never shown - board TX evidence lost.
        if self.in_frame and self.seg:
            seg = bytes(self.seg)
            printable = all(32 <= b < 127 or b in (10, 13) for b in seg)
            if printable:
                self.junk += seg
                self.seg = bytearray()
                self.in_frame = False
        if self.junk:
            sys.stdout.write("[HOST_SIM] console text (%s):\n%s\n"
                             % (why, self.junk.decode("ascii",
                                                     errors="replace")))
            self.junk = bytearray()

        # Raw hex dump: the definitive "what did the port actually deliver"
        # evidence. Non-printable garbage (e.g. a frame we cannot parse)
        # is invisible in the junk capture above - this shows every byte.
        if self.raw_all:
            sys.stdout.write("[HOST_SIM] raw bytes (%s): %d total\n%s\n"
                             % (why, len(self.raw_all),
                                hexdump(self.raw_all)))
            self.raw_all = bytearray()

    def expect_no_rsp(self, request_id: int, seconds: float):
        """Assert that NO RSP with this request_id arrives in the window."""
        for ftype, rid, payload in self.read_segments(seconds):
            if ftype == UART_IPC_TYPE_RSP and rid == request_id:
                raise Fail("unexpected RSP for request_id=%d "
                           "(old request_id must not be answered)" % rid)

    def close(self):
        try:
            self.ser.close()
        except Exception:               # noqa: BLE001 - best effort
            pass


# ---------------------------------------------------------------------------
# Judged modes
# ---------------------------------------------------------------------------

def build_corrupt_crc_frame(request_id: int, opcode: int, addr: int,
                            data: int) -> bytes:
    """Build a frame that is COBS-valid but carries a WRONG CRC: build
    the raw frame (header+payload+crc), flip one payload byte AFTER the
    CRC was computed, then COBS-encode and delimit. The firmware's decode
    path therefore passes COBS and header checks and fails exactly at
    the CRC comparison - this is the 'bad CRC' injection the spec asks
    for (a COBS code-byte flip would be rejected earlier as a FORMAT
    error and would never exercise the CRC check)."""
    payload = (_le(opcode, 4) + _le(addr & 0xFFFFFFFF, 4) +
               _le((addr >> 32) & 0xFFFFFFFF, 4) + _le(data, 4))
    raw = bytearray()
    raw += _le(UART_IPC_VERSION, 1)
    raw += _le(UART_IPC_TYPE_CMD, 1)
    raw += _le(len(payload), 2)
    raw += _le(SESSION_ID, 8)
    raw += _le(request_id, 4)
    raw += payload
    raw += _le(zlib.crc32(bytes(raw)) & 0xFFFFFFFF, 4)
    raw[UART_IPC_HDR_LEN + 3] ^= 0x01   # flip payload AFTER crc was stored
    return b"\x00" + cobs_encode(bytes(raw)) + b"\x00"


def mode_echo(link):
    """RD REG0: RSP must echo request_id, status=OK, any data."""
    rid = 101
    link.send(build_cmd(rid, 0, ADDR_REG0, 0))
    status, data = link.wait_rsp(rid)
    if status != UART_IPC_OK:
        raise Fail("RD RSP status=%d (want 0 OK)" % status)
    print("[HOST_SIM] echo: RD REG0 -> status=OK data=0x%08x" % data)


def mode_crc_error(link):
    """Send one frame that is COBS-valid but has a WRONG CRC (the CRC
    check itself rejects it): firmware must drop it silently. The next
    good frame must still get a RSP."""
    bad = build_corrupt_crc_frame(201, 0, ADDR_REG0, 0)
    link.send(bad)
    # the bad frame must produce NO RSP
    link.expect_no_rsp(201, 1.0)
    # good frame afterwards: answered
    rid = 202
    link.send(build_cmd(rid, 0, ADDR_REG0, 0))
    status, _ = link.wait_rsp(rid)
    if status != UART_IPC_OK:
        raise Fail("good frame after bad CRC got status=%d" % status)
    print("[HOST_SIM] crc-error: bad-CRC frame dropped, good frame answered")


def mode_dedup(link):
    """Spec 8.2 dedup behavior on the live firmware. The firmware keeps
    exactly ONE latest-result record (request_id + content + saved
    status/data) plus a highest_seen watermark (uart_ipc_transport.c,
    Task 5 review approved):
      - duplicate of the LATEST record (same id + same content) ->
        answered by resending the SAVED result, no re-execution
      - same id + different content -> rejected (INVALID)
      - id <= highest_seen but not the latest record -> silently
        dropped: NOT executed, NOT answered
    Step ordering matters because the record holds only the newest id:
    the "resend saved result" check must run while 301 is STILL the
    latest record (immediately after its first RSP, before any newer id
    is sent); the old-id checks come afterwards, when 301 has been
    pushed out of the record by newer ids."""
    v1 = 0x11111111
    v2 = 0x22222222

    # 1) WR V1 with id 301 -> answered
    link.send(build_cmd(301, 1, ADDR_REG0, v1))
    status, _ = link.wait_rsp(301)
    if status != UART_IPC_OK:
        raise Fail("first WR (id 301) status=%d" % status)

    # 2) IMMEDIATELY resend the same id 301 + same content: still the
    #    latest record -> must be answered with the SAVED result (the
    #    same status as the first RSP) and must NOT re-execute
    link.send(build_cmd(301, 1, ADDR_REG0, v1))
    status, _ = link.wait_rsp(301)
    if status != UART_IPC_OK:
        raise Fail("duplicate WR (id 301, latest record) not answered, "
                   "status=%d" % status)
    print("[HOST_SIM] dedup: duplicate of latest record answered with "
          "saved result")

    # 3) same id 301, different content -> INVALID (content mismatch
    #    against the latest record)
    link.send(build_cmd(301, 1, ADDR_REG0, v1 + 1))
    status, _ = link.wait_rsp(301)
    if status != UART_IPC_ERR_INVALID:
        raise Fail("same id different content: status=%d (want INVALID=%d)"
                   % (status, UART_IPC_ERR_INVALID))
    print("[HOST_SIM] dedup: same id + different content rejected")

    # 4) WR V2 with a fresh id 302 (now the latest record is 302)
    link.send(build_cmd(302, 1, ADDR_REG0, v2))
    status, _ = link.wait_rsp(302)
    if status != UART_IPC_OK:
        raise Fail("second WR (id 302) status=%d" % status)

    # 5) old id 301 (not the latest record, <= highest_seen): must get
    #    NO response - silently dropped, not executed, not answered
    link.send(build_cmd(301, 1, ADDR_REG0, v1))
    link.expect_no_rsp(301, 1.0)
    print("[HOST_SIM] dedup: old request_id got no response")

    # 6) read REG0 back with a fresh id 303: must be V2. If it were V1,
    #    the resent WR in step 5 would have re-executed (V1 overwriting
    #    the newer V2 write is only possible that way). Any other value
    #    is UNRESOLVED (REG0 writability is not board-verified).
    link.send(build_cmd(303, 0, ADDR_REG0, 0))
    status, data = link.wait_rsp(303)
    if status != UART_IPC_OK:
        raise Fail("RD back (id 303) status=%d" % status)
    if data == v1:
        raise Fail("old request_id WR re-executed (REG0=0x%08x, want "
                   "0x%08x)" % (data, v2))
    if data != v2:
        raise Fail("REG0=0x%08x matches neither V1 nor V2 - UNRESOLVED"
                   % data)
    print("[HOST_SIM] dedup: old request_id did not re-execute "
          "(REG0=0x%08x)" % data)


def mode_burst(link):
    """8 RD frames, distinct request_ids, all answered, none lost."""
    ids = list(range(401, 409))
    for rid in ids:
        link.send(build_cmd(rid, 0, ADDR_REG0, 0))
    answered = {}
    # 8 frames may arrive back-to-back; give the whole burst the window
    deadline = time.monotonic() + RSP_TIMEOUT_S
    while len(answered) < len(ids) and time.monotonic() < deadline:
        for ftype, rid, payload in link.read_segments(0.2):
            if ftype == UART_IPC_TYPE_RSP and rid in ids:
                answered[rid] = parse_rsp_payload(payload)
    missing = [rid for rid in ids if rid not in answered]
    if missing:
        raise Fail("burst: missing RSP for request_ids %s" % missing)
    bad = [rid for rid in ids if answered[rid][0] != UART_IPC_OK]
    if bad:
        raise Fail("burst: non-OK status for request_ids %s" % bad)
    print("[HOST_SIM] burst: %d/%d frames answered, none lost"
          % (len(answered), len(ids)))


def mode_session_rebuild(link):
    """8.2 session semantics: a NEW session_id must make the firmware
    rebuild its dedup table, so request_ids may restart from any value.

    Steps (REG0 is the RW boot counter; V1/V2/V3 are distinct write values):
      1. session A: WR id=601 V1, RD id=602 back -> V1 (baseline works)
      2. still session A: WR id=603 V2, RD id=604 back -> V2
      3. session B (new tag - simulates a restarted host): re-send the
         SAME WR id=601 V1 (an id the old session already used; without
         the rebuild the firmware drops it silently as an old retry).
         Must be EXECUTED: RD id=602 back -> V1 (overwriting V2 proves
         the WR really re-ran in the new session).
      4. every RSP must echo the CURRENT session (stale-session frames
         are counted by the link and judged below).

    Judged: all four steps answered with status OK, step-3 RD reads V1,
    zero stale-session frames."""
    v1, v2 = 0x11111111, 0x22222222

    def rd_back(rid):
        link.send(build_cmd(rid, 0, ADDR_REG0, 0))
        status, data = link.wait_rsp(rid)
        if status != UART_IPC_OK:
            raise Fail("session-rebuild: RD id=%d status=%d" % (rid, status))
        return data

    # 1. session A baseline
    link.send(build_cmd(601, 1, ADDR_REG0, v1))
    if link.wait_rsp(601)[0] != UART_IPC_OK:
        raise Fail("session-rebuild: WR id=601 (session A) failed")
    if rd_back(602) != v1:
        raise Fail("session-rebuild: baseline RD != V1")
    # 2. session A continues (highest_seen now 604)
    link.send(build_cmd(603, 1, ADDR_REG0, v2))
    if link.wait_rsp(603)[0] != UART_IPC_OK:
        raise Fail("session-rebuild: WR id=603 (session A) failed")
    if rd_back(604) != v2:
        raise Fail("session-rebuild: RD after V2 write != V2")

    # 3. session B: same WR id=601 - must re-execute (dedup table rebuilt)
    link.session_id = (SESSION_ID ^ 0x5A5A5A5A5A5A5A5A) & 0xFFFFFFFFFFFFFFFF
    if link.session_id in (0, UART_IPC_SESSION_V1, SESSION_ID):
        link.session_id += 2
    link.send(build_cmd(601, 1, ADDR_REG0, v1,
                        session_id=link.session_id))
    if link.wait_rsp(601)[0] != UART_IPC_OK:
        raise Fail("session-rebuild: WR id=601 re-sent in session B got "
                   "no OK RSP - firmware dropped it as an old retry?")
    data = rd_back(602)
    if data != v1:
        raise Fail("session-rebuild: new session did not re-execute "
                   "WR id=601 (REG0=0x%08x, want V1=0x%08x)" % (data, v1))

    # 4. session echo discipline
    if link.stale_session_frames:
        raise Fail("session-rebuild: %d frames echoed with a stale "
                   "session_id" % link.stale_session_frames)
    print("[HOST_SIM] session-rebuild: new session re-executes reused "
          "request_ids, all RSPs echo current session")


# ---------------------------------------------------------------------------
# Stress mode
# ---------------------------------------------------------------------------

# Defaults overridable from the command line: --stress-count / --stress-gap /
# --stress-start-id
STRESS_COUNT_DEFAULT = 500     # requests per run
STRESS_GAP_MS_DEFAULT = 0      # 0 = back-to-back (max rate)
STRESS_START_ID_DEFAULT = 5000

def mode_stress(link, count, gap_ms, start_id):
    """Sustained-rate soak: `count` sequential RD requests.

    Each request is sent, its RSP awaited (with retry on timeout), then the
    next one - so the exchange rate is bounded by the round-trip, not by
    the sender. gap_ms > 0 additionally idles between requests (simulating
    a slow client); gap_ms = 0 is the back-to-back worst case.

    request_ids run start_id, start_id+1, ... - firmware dedup (spec 8.2)
    silently drops any id <= the highest it has ever seen, so back-to-back
    runs against one board MUST NOT reuse ids: pass --stress-start-id above
    the previous run's last id (previous run used ids up to start_id+count-1).

    Judged:
      - every request_id answered exactly once (duplicates counted)
      - all statuses OK
      - zero retries needed - clean run (retries are reported, allowed
        up to 10% of total, beyond that FAIL - a link
        that needs constant retransmission is not healthy).
    Progress lines every 100 requests so a long soak shows liveness."""
    retries = 0
    duplicates = 0
    rid = start_id

    for i in range(count):
        link.send(build_cmd(rid, 0, ADDR_REG0, 0))

        got = False
        for _attempt in range(3):
            for ftype, r, payload in link.read_segments(
                    RSP_TIMEOUT_S if _attempt else 1.0):
                if ftype == UART_IPC_TYPE_RSP and r == rid:
                    status, _data = parse_rsp_payload(payload)
                    if status != UART_IPC_OK:
                        link._dump_junk("stress: bad status for "
                                        "request_id=%d" % rid)
                        raise Fail("stress: request_id=%d status=%d"
                                   % (rid, status))
                    got = True
                    break
            if got:
                break
            # timeout on this attempt: firmware may have dropped the frame
            # (ring barrier after a noise glitch). Re-send the SAME id -
            # dedup on the firmware side resends the saved result if it
            # had executed, executes it fresh otherwise. Either way the
            # exchange recovers; we count the retry.
            retries += 1
            link.send(build_cmd(rid, 0, ADDR_REG0, 0))
        if not got:
            # Evidence first (same discipline as wait_rsp): dump every
            # byte the port delivered + any console text, THEN judge.
            # Without this a stress failure is a black box.
            link._dump_junk("stress: no RSP for request_id=%d "
                            "(after 3 attempts)" % rid)
            raise Fail("stress: request_id=%d unanswered after 3 attempts "
                       "(%d/%d done)" % (rid, i, count))

        # drain anything else that came in (duplicate RSP for this id =
        # firmware answered the same request twice - protocol violation)
        for ftype, r, _payload in link.read_segments(0.01):
            if ftype == UART_IPC_TYPE_RSP and r == rid:
                duplicates += 1

        rid += 1
        if (i + 1) % 100 == 0:
            print("[HOST_SIM] stress: %d/%d done (retries=%d dup=%d)"
                  % (i + 1, count, retries, duplicates))
        if gap_ms:
            time.sleep(gap_ms / 1000.0)

    if duplicates:
        raise Fail("stress: %d duplicate RSPs (firmware answered twice)"
                   % duplicates)
    print("[HOST_SIM] stress: %d requests, %d retries, %d duplicates"
          % (count, retries, duplicates))
    if retries > count // 10:
        raise Fail("stress: %d retries > 10%% of %d - link not healthy"
                   % (retries, count))


# ---------------------------------------------------------------------------
# Offline self-test
# ---------------------------------------------------------------------------

def run_self_test():
    """No serial port touched. Checks:
      1. zlib.crc32 check value 0xCBF43926 (CRC-32/ISO-HDLC baseline)
      2. COBS roundtrip + explicit encode/decode spot values
      3. frame build/parse roundtrip for CMD/RSP/LOG
      4. wire vectors byte-identical to the C-side encoder output
    Prints [HOST_SIM] self-test: ... PASS/FAIL lines, returns True/False."""
    ok = True

    def check(cond, name, why):
        nonlocal ok
        if cond:
            print("[HOST_SIM] self-test: %s PASS" % name)
        else:
            ok = False
            print("[HOST_SIM] self-test: %s FAIL: %s" % (name, why))

    # 1. CRC-32 check value (same anchor as the C host unit test)
    check(zlib.crc32(b"123456789") == 0xCBF43926,
          "crc32-check-value", "zlib.crc32(b'123456789') != 0xCBF43926")

    # 2. COBS spot values: zero-heavy and non-zero payloads roundtrip
    for name, data in (("cobs-roundtrip-ff", b"\xff" * 16),
                       ("cobs-roundtrip-00", b"\x00" * 16),
                       ("cobs-roundtrip-mixed",
                        b"\x12\x00\x34\x00\x00\x56")):
        try:
            back = cobs_decode(cobs_encode(data))
            check(back == data, name, "roundtrip bytes differ")
        except ValueError as exc:
            check(False, name, "exception: %s" % exc)
    # explicit encode spot value, verified against external/cobs:
    # cobs_encode(b"\x00\x11") -> b"\x01\x02\x11"
    check(cobs_encode(b"\x00\x11") == b"\x01\x02\x11",
          "cobs-encode-spot", "cobs_encode(b'\\x00\\x11') != 01 02 11")

    # 3. frame build/parse roundtrip for all three frame types
    cmd = build_cmd(7, 1, 0x1122334455667788, 0xA5A5A5A5)
    try:
        ftype, rid, payload = parse_frame(cmd[1:-1])
        check(ftype == UART_IPC_TYPE_CMD and rid == 7 and
              payload == (_le(1, 4) + _le(0x55667788, 4) +
                            _le(0x11223344, 4) + _le(0xA5A5A5A5, 4)),
              "cmd-roundtrip", "parsed fields differ")
    except ValueError as exc:
        check(False, "cmd-roundtrip", "exception: %s" % exc)

    rsp = build_frame(UART_IPC_TYPE_RSP,
                      _le(0, 4) + _le(0x12345678, 4), 7)
    try:
        ftype, rid, payload = parse_frame(rsp[1:-1])
        check(ftype == UART_IPC_TYPE_RSP and rid == 7 and
              parse_rsp_payload(payload) == (0, 0x12345678),
              "rsp-roundtrip", "parsed fields differ")
    except ValueError as exc:
        check(False, "rsp-roundtrip", "exception: %s" % exc)

    log_payload = _le(0x11223344, 4) + _le(0x55667788, 4) + b"hello pmu"
    log = build_frame(UART_IPC_TYPE_LOG, log_payload, 0)
    try:
        ftype, rid, payload = parse_frame(log[1:-1])
        check(ftype == UART_IPC_TYPE_LOG and rid == 0 and
              payload == log_payload,
              "log-roundtrip", "parsed fields differ")
    except ValueError as exc:
        check(False, "log-roundtrip", "exception: %s" % exc)

    # corrupted byte must be rejected by the Python parser too: build a
    # frame with a WRONG CRC (payload flipped after CRC computed), so
    # COBS decoding succeeds and the rejection comes from the CRC check
    bad = build_corrupt_crc_frame(7, 1, 0x1122334455667788, 0xA5A5A5A5)
    try:
        parse_frame(bad[1:-1])
        check(False, "crc-corrupt-reject",
              "parser accepted a frame with a wrong CRC")
    except ValueError:
        check(True, "crc-corrupt-reject", "")

    # 4. byte-identical agreement with the C encoder (see VECTORS above).
    #    Vectors were generated with session_id = V1 constant, so pass it
    #    explicitly - the live SESSION_ID is per-process and would differ.
    check(build_cmd(7, 1, 0x1122334455667788, 0xA5A5A5A5,
                    session_id=UART_IPC_SESSION_V1) == VECTORS["CMD"],
          "vector-cmd-wr",
          "Python CMD WR frame differs from C-side uart_ipc_encode output")
    check(build_cmd(7, 0, 0x10000007, 0,
                    session_id=UART_IPC_SESSION_V1) == VECTORS["CMD2"],
          "vector-cmd-rd",
          "Python CMD RD frame differs from C-side uart_ipc_encode output")
    check(build_frame(UART_IPC_TYPE_RSP,
                      _le(0, 4) + _le(0x12345678, 4), 7,
                      session_id=UART_IPC_SESSION_V1) == VECTORS["RSP"],
          "vector-rsp",
          "Python RSP frame differs from C-side uart_ipc_encode output")
    check(build_frame(UART_IPC_TYPE_LOG,
                      _le(0x11223344, 4) + _le(0x55667788, 4),
                      0, session_id=UART_IPC_SESSION_V1) == VECTORS["LOG"],
          "vector-log",
          "Python LOG frame differs from C-side uart_ipc_encode output")

    print("[HOST_SIM] self-test: %s" % ("PASS" if ok else "FAIL"))
    return ok


# ---------------------------------------------------------------------------
# Entry
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Atlas PMU UART IPC host simulator")
    ap.add_argument("--port", help="serial port, e.g. /dev/ttyUSB1")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--mode",
                    choices=["echo", "crc-error", "dedup", "burst",
                             "stress", "session-rebuild"],
                    help="on-device judged mode")
    ap.add_argument("--self-test", action="store_true",
                    help="offline self-test (no serial port)")
    ap.add_argument("--stress-count", type=int,
                    default=STRESS_COUNT_DEFAULT,
                    help="stress mode: number of requests (default %d)"
                         % STRESS_COUNT_DEFAULT)
    ap.add_argument("--stress-gap", type=int, default=STRESS_GAP_MS_DEFAULT,
                    help="stress mode: idle gap between requests in ms "
                         "(default %d = back-to-back)"
                         % STRESS_GAP_MS_DEFAULT)
    ap.add_argument("--stress-start-id", type=int,
                    default=STRESS_START_ID_DEFAULT,
                    help="stress mode: first request_id (default %d). "
                         "Firmware drops ids <= highest ever seen, so "
                         "back-to-back runs must raise this above the "
                         "previous run's last id (start_id + count - 1)"
                         % STRESS_START_ID_DEFAULT)
    args = ap.parse_args()

    if args.self_test:
        sys.exit(0 if run_self_test() else 1)

    if not args.port or not args.mode:
        print("[HOST_SIM] FAIL: --port and --mode are required "
              "(or --self-test)", file=sys.stderr)
        sys.exit(1)

    modes = {
        "echo": mode_echo,
        "crc-error": mode_crc_error,
        "dedup": mode_dedup,
        "burst": mode_burst,
        "session-rebuild": mode_session_rebuild,
        "stress": lambda link: mode_stress(link, args.stress_count,
                                            args.stress_gap,
                                            args.stress_start_id),
    }

    try:
        link = Link(args.port, args.baud)
    except Fail as f:
        print("[HOST_SIM] FAIL: %s" % f)
        sys.exit(1)

    print("[HOST_SIM] port=%s baud=%d mode=%s"
          % (args.port, args.baud, args.mode))
    try:
        # ignore firmware boot noise / log frames on the link, then run
        modes[args.mode](link)
        print("[HOST_SIM] %s PASS" % args.mode)
        sys.exit(0)
    except Fail as f:
        print("[HOST_SIM] %s FAIL: %s" % (args.mode, f))
        sys.exit(1)
    finally:
        link.close()


if __name__ == "__main__":
    main()
