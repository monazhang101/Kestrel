/* UART IPC RX state machine implementation – see uart_ipc_rx.h.
 *
 * Plain words: HUNT = looking for a frame start; COLLECT = keeping
 * frame bytes; DISCARD_TO_ZERO = an oversized frame is being thrown
 * away, byte by byte, until the next 0x00.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "uart_ipc_rx.h"

void uart_ipc_rx_init(uart_ipc_rx_t *rx)
{
    memset(rx, 0, sizeof *rx);
    rx->state = UART_IPC_RX_HUNT;
}

/* Throw away the half-built frame, back to HUNT. Error counters are
 * intentionally NOT touched (gap/idle_reset contract). */
static void clear_half_frame(uart_ipc_rx_t *rx)
{
    rx->len = 0u;
    rx->state = UART_IPC_RX_HUNT;
}

/* Same byte-level clear as clear_half_frame – separate name because the
 * caller-facing contract (idle timeout) is documented separately. */
void uart_ipc_rx_idle_reset(uart_ipc_rx_t *rx)
{
    clear_half_frame(rx);
}

void uart_ipc_rx_gap(uart_ipc_rx_t *rx)
{
    clear_half_frame(rx);
}

void uart_ipc_rx_feed(uart_ipc_rx_t *rx, const uint8_t *data, size_t n,
                      uart_ipc_rx_frame_cb cb, void *ctx)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        uint8_t byte = data[i];

        if (byte == 0u) {
            /* Frame boundary. In DISCARD_TO_ZERO the 0x00 ends the
             * oversized frame: swallow it and hunt again - no callback,
             * its bytes are already gone. */
            if (rx->state == UART_IPC_RX_DISCARD_TO_ZERO) {
                rx->state = UART_IPC_RX_HUNT;
                rx->len = 0u;
                continue;
            }
            /* Non-empty buffer = one complete frame. Hand it to the
             * caller's callback; the state machine does not decode.
             * An empty buffer (HUNT, or COLLECT right after a frame)
             * is just a stray delimiter: nothing to report. */
            if (rx->len > 0u && cb != NULL) {
                cb(rx->buf, rx->len, ctx);
            }
            rx->len = 0u;
            rx->state = UART_IPC_RX_HUNT;
        } else if (rx->state == UART_IPC_RX_DISCARD_TO_ZERO) {
            /* Leftover bytes of the oversized frame: drop silently. */
        } else if (rx->len < sizeof rx->buf) {
            rx->buf[rx->len] = byte;
            rx->len++;
            rx->state = UART_IPC_RX_COLLECT;
        } else {
            /* No room left: drop the half frame, count it, and throw
             * away everything up to the next 0x00. */
            rx->len = 0u;
            rx->state = UART_IPC_RX_DISCARD_TO_ZERO;
            rx->dropped_oversize++;
        }
    }
}
