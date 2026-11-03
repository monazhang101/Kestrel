/* UART IPC protocol layer – frame encode/decode over COBS + CRC32.
 *
 * Frame layout on the wire:
 *
 *   0x00 | COBS(header 16B + payload 8..256B + crc 4B) | 0x00
 *
 * - All multi-byte fields are little-endian, encoded/decoded one field
 *   at a time (no struct memcpy, avoids alignment/padding issues).
 * - CRC-32/ISO-HDLC (zlib compatible) over header + payload.
 * - Error codes numerically match common/ipc_rw IPC_ERR_* (defined
 *   locally, no include of ipc_rw).
 *
 * Host unit test: pmu/test/host/uart_ipc_proto_host_test.c.
 */
#ifndef UART_IPC_PROTO_H
#define UART_IPC_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define UART_IPC_VERSION             1u
#define UART_IPC_MAX_PAYLOAD         256u
#define UART_IPC_HDR_LEN             16u
#define UART_IPC_CRC_LEN             4u
/* raw = header + payload + crc */
#define UART_IPC_MAX_RAW  (UART_IPC_HDR_LEN + UART_IPC_MAX_PAYLOAD + UART_IPC_CRC_LEN)  /* 276 */
#define UART_IPC_MAX_ENCODED         278u  /* COBS_ENCODE_DST_BUF_LEN_MAX(276) */
#define UART_IPC_MAX_WIRE            280u  /* leading 0x00 + encoded + trailing 0x00 */

typedef enum {
    UART_IPC_TYPE_CMD = 1,
    UART_IPC_TYPE_RSP = 2,
    UART_IPC_TYPE_LOG = 3,
} uart_ipc_type_t;

/* Error codes: values match common/ipc_rw IPC_ERR_* (local, no ipc_rw include) */
enum {
    UART_IPC_OK          = 0,
    UART_IPC_ERR_INVALID = 1 << 0,    /* == IPC_ERR_INVALID */
    UART_IPC_ERR_TIMEOUT = 1 << 1,    /* == IPC_ERR_TIMEOUT */
    UART_IPC_ERR_UNKNOWN = 1 << 2,    /* == IPC_ERR_UNKNOWN */
    /* UART 通道级状态码: 8.2 语义里“通道忙，命令没投出去”是通道级
     * 最终结果（ipc_rw 总线语义无此码——那边靠 seq/ack 重试，不存在
     * “被服务方拒绝”），所以只定义在 UART 通道头这里，数值 1<<3 与
     * 上面三个错误不冲突。 */
    UART_IPC_ERR_BUSY    = 1 << 3,
};

typedef struct {
    uint8_t  version;
    uint8_t  type;        /* uart_ipc_type_t */
    uint16_t payload_len; /* exact length: CMD=16, RSP=8, LOG=8..256 */
    uint64_t session_id;  /* V1 fixed UART_IPC_SESSION_V1 */
    uint32_t request_id;  /* CMD/RSP non-zero; LOG is 0 */
} uart_ipc_hdr_t;

#define UART_IPC_SESSION_V1  0x0000000100000001ULL

/* CMD payload 16B: opcode uses IPC_OP_RD=0 / IPC_OP_WR=1 semantics */
typedef struct {
    uint32_t opcode;      /* 0=RD 1=WR (aligned to ipc_rw op_code_t) */
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t data;        /* WR: value to write; RD: 0 */
} uart_ipc_cmd_t;

/* RSP payload 8B: status reuses UART_IPC_ERR_* (== IPC_ERR_*) */
typedef struct {
    uint32_t status;
    uint32_t data;        /* RD: value read; WR: 0 */
} uart_ipc_rsp_t;

/* Encode: hdr+payload -> raw(header+payload+crc, LE field by field)
 * -> COBS -> wire(leading 0x00 + encoded + trailing 0x00).
 * cap = wire buffer capacity (must be >= UART_IPC_MAX_WIRE).
 * Returns UART_IPC_OK or UART_IPC_ERR_INVALID. */
int uart_ipc_encode(const uart_ipc_hdr_t *hdr, const uint8_t *payload,
                    uint8_t *wire, size_t cap, size_t *wire_len);

/* Decode: input raw is the de-COBSed frame (header+payload+crc, no
 * COBS bytes / delimiters). Checks version, length direction, payload
 * exact length per type, CRC. Returns UART_IPC_OK or UART_IPC_ERR_INVALID. */
int uart_ipc_decode(const uint8_t *raw, size_t raw_len,
                    uart_ipc_hdr_t *hdr, uint8_t *payload, size_t payload_cap);

#endif /* UART_IPC_PROTO_H */
