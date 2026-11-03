/* UART IPC protocol layer – frame encode/decode over COBS + CRC32.
 *
 * See uart_ipc_proto.h for the frame layout and the exact signatures
 * later tasks depend on. Field-by-field LE coding only – no struct
 * memcpy (avoids alignment/padding issues).
 *
 * Host unit test: pmu/test/host/uart_ipc_proto_host_test.c.
 */
#include <string.h>
#include "uart_ipc_proto.h"
#include "cobs.h"
#include "crc32.h"

/* ---- little-endian field packing helpers ---- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (uint32_t i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xffu);
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] |
                      ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0u;
    for (uint32_t i = 0; i < 8u; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

/* ---- shared checks ---- */

/* Does this (type, payload_len) pair match the protocol's exact-length
 * rules? CMD=16, RSP=8, LOG=8..256. */
static int payload_len_matches_type(uint8_t type, uint16_t payload_len)
{
    switch (type) {
    case UART_IPC_TYPE_CMD:
        return payload_len == 16u;
    case UART_IPC_TYPE_RSP:
        return payload_len == 8u;
    case UART_IPC_TYPE_LOG:
        return payload_len >= 8u && payload_len <= UART_IPC_MAX_PAYLOAD;
    default:
        return 0;
    }
}

/* ---- encode ---- */

int uart_ipc_encode(const uart_ipc_hdr_t *hdr, const uint8_t *payload,
                    uint8_t *wire, size_t cap, size_t *wire_len)
{
    uint8_t raw[UART_IPC_MAX_RAW];
    uint8_t encoded[UART_IPC_MAX_ENCODED];
    cobs_encode_result enc;

    /* 1. Static checks: version, type, exact payload length per type,
     *    session/request rules. */
    if (hdr == NULL || payload == NULL || wire == NULL || wire_len == NULL) {
        return UART_IPC_ERR_INVALID;
    }
    if (hdr->version != UART_IPC_VERSION) {
        return UART_IPC_ERR_INVALID;
    }
    if (hdr->type != UART_IPC_TYPE_CMD && hdr->type != UART_IPC_TYPE_RSP &&
        hdr->type != UART_IPC_TYPE_LOG) {
        return UART_IPC_ERR_INVALID;
    }
    if (!payload_len_matches_type(hdr->type, hdr->payload_len)) {
        return UART_IPC_ERR_INVALID;
    }
    /* session_id 是“当前会话标签”（8.2：值非 0 即可，值变化=新会话，
     * 由 transport 层比对并触发去重表重建）。协议层不再校验其具体
     * 值—V1 时代的“恒等于 UART_IPC_SESSION_V1”校验已随会话语义
     * 启用而移除。0 保留（非法，encode/decode 双向拒绝）。 */
    if (hdr->session_id == 0u) {
        return UART_IPC_ERR_INVALID;
    }
    if (hdr->type == UART_IPC_TYPE_LOG) {
        if (hdr->request_id != 0u) {
            return UART_IPC_ERR_INVALID;
        }
    } else {
        if (hdr->request_id == 0u) {
            return UART_IPC_ERR_INVALID;
        }
    }

    /* 2. Field-by-field raw build: header, payload, CRC (LE). */
    raw[0] = hdr->version;
    raw[1] = hdr->type;
    put_u16(&raw[2], hdr->payload_len);
    put_u64(&raw[4], hdr->session_id);
    put_u32(&raw[12], hdr->request_id);
    memcpy(&raw[UART_IPC_HDR_LEN], payload, hdr->payload_len);
    put_u32(&raw[UART_IPC_HDR_LEN + hdr->payload_len],
            crc32_ieee(raw, UART_IPC_HDR_LEN + hdr->payload_len));

    /* 3. COBS encode. */
    enc = cobs_encode(encoded, sizeof encoded, raw,
                      UART_IPC_HDR_LEN + hdr->payload_len + UART_IPC_CRC_LEN);
    if (enc.status != COBS_ENCODE_OK) {
        return UART_IPC_ERR_INVALID;
    }

    /* 4. Wrap with the 0x00 delimiters, after the capacity check. */
    if (cap < enc.out_len + 2u) {
        return UART_IPC_ERR_INVALID;
    }
    wire[0] = 0u;
    memcpy(&wire[1], encoded, enc.out_len);
    wire[1u + enc.out_len] = 0u;
    *wire_len = enc.out_len + 2u;
    return UART_IPC_OK;
}

/* ---- decode ---- */

int uart_ipc_decode(const uint8_t *raw, size_t raw_len,
                    uart_ipc_hdr_t *hdr, uint8_t *payload, size_t payload_cap)
{
    if (raw == NULL || hdr == NULL || payload == NULL) {
        return UART_IPC_ERR_INVALID;
    }
    /* Length direction: must at least hold header + crc. */
    if (raw_len < UART_IPC_HDR_LEN + UART_IPC_CRC_LEN) {
        return UART_IPC_ERR_INVALID;
    }

    /* Field-by-field header read. */
    hdr->version     = raw[0];
    hdr->type        = raw[1];
    hdr->payload_len = get_u16(&raw[2]);
    hdr->session_id  = get_u64(&raw[4]);
    hdr->request_id  = get_u32(&raw[12]);

    if (hdr->version != UART_IPC_VERSION) {
        return UART_IPC_ERR_INVALID;
    }
    /* session_id 会话语义（见 encode 侧同名注释）：非 0 即可，值由
     * transport 层比对。0 拒绝。 */
    if (hdr->session_id == 0u) {
        return UART_IPC_ERR_INVALID;
    }
    /* Payload exact length per type. */
    if (!payload_len_matches_type(hdr->type, hdr->payload_len)) {
        return UART_IPC_ERR_INVALID;
    }
    /* Length direction: raw must be exactly header + payload + crc. */
    if (raw_len != UART_IPC_HDR_LEN + hdr->payload_len + UART_IPC_CRC_LEN) {
        return UART_IPC_ERR_INVALID;
    }
    if (payload_cap < hdr->payload_len) {
        return UART_IPC_ERR_INVALID;
    }

    /* CRC over header + payload, compared against the stored LE value. */
    if (get_u32(&raw[UART_IPC_HDR_LEN + hdr->payload_len]) !=
        crc32_ieee(raw, UART_IPC_HDR_LEN + hdr->payload_len)) {
        return UART_IPC_ERR_INVALID;
    }

    memcpy(payload, &raw[UART_IPC_HDR_LEN], hdr->payload_len);
    return UART_IPC_OK;
}
