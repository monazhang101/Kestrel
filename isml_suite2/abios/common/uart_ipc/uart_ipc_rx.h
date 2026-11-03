/* UART IPC RX state machine – finds 0x00-delimited frame boundaries in a
 * byte stream (HUNT/COLLECT).
 *
 * The state machine only splits frames; it does NOT decode them. On a
 * frame boundary it hands the accumulated bytes to the caller's
 * callback, which runs COBS decode + uart_ipc_decode.
 *
 * Half frames live in a fixed buffer of UART_IPC_MAX_ENCODED bytes. A
 * "frame" longer than that is dropped and counted; the receiver then
 * discards bytes until the next 0x00 before hunting again.
 *
 * Host unit test: pmu/test/host/uart_ipc_rx_host_test.c.
 */
#ifndef UART_IPC_RX_H
#define UART_IPC_RX_H

#include <stdint.h>
#include <stddef.h>
#include "uart_ipc_proto.h"

/* Internal states: HUNT waits for a 0x00 delimiter; COLLECT gathers
 * frame bytes until the next 0x00; DISCARD_TO_ZERO throws away an
 * oversized frame's remainder up to the next 0x00. */
enum {
    UART_IPC_RX_HUNT = 0,
    UART_IPC_RX_COLLECT = 1,
    UART_IPC_RX_DISCARD_TO_ZERO = 2,
};

typedef struct {
    int      state;               /* UART_IPC_RX_HUNT / _COLLECT / _DISCARD_TO_ZERO */
    uint8_t  buf[UART_IPC_MAX_ENCODED];  /* half-built frame, hard size cap */
    size_t   len;
    /* Error counters (plain names, for reading/printing by upper layer). */
    uint32_t dropped_oversize;   /* half frame exceeded the buffer, dropped */
    uint32_t dropped_bad_frame;  /* frame rejected by decode/CRC, dropped */
} uart_ipc_rx_t;

/* Raw = wire bytes between two 0x00 delimiters (still COBS-encoded
 * header+payload+crc). The state machine does not call
 * uart_ipc_decode – the upper layer does that inside the callback. */
typedef void (*uart_ipc_rx_frame_cb)(const uint8_t *raw, size_t raw_len,
                                     void *ctx);

/* Clear everything including the error counters. */
void uart_ipc_rx_init(uart_ipc_rx_t *rx);

/* Feed n bytes of the received stream. On each 0x00: if the buffer holds
 * bytes, fire the callback with them once, then empty the buffer. While
 * COLLECTing, a byte that no longer fits the buffer (len >= sizeof buf)
 * makes the machine drop the frame, count it, and discard everything up
 * to the next 0x00. */
void uart_ipc_rx_feed(uart_ipc_rx_t *rx, const uint8_t *data, size_t n,
                      uart_ipc_rx_frame_cb cb, void *ctx);

/* The byte stream broke (RX error / ring full barrier): throw away the
 * half-built frame and go back to HUNT. Clears only buf/len/state, NOT
 * the error counters. */
void uart_ipc_rx_gap(uart_ipc_rx_t *rx);

/* Half frame idle timeout: same byte-level behavior as gap(). Whether
 * the half frame counts as "dropped" is decided by the upper layer
 * (spec §5.8: a timeout alone does not clear dedup state, and dedup
 * state does not live in this struct). */
void uart_ipc_rx_idle_reset(uart_ipc_rx_t *rx);

#endif /* UART_IPC_RX_H */
