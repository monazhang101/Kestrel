/* CRC-32/ISO-HDLC (IEEE 802.3, zlib compatible) – shared CRC helper.
 *
 * Implementation copied from bootrom/src/core/boot_seq.c (crc32_ieee,
 * 16-entry nibble table). Only change: made non-static and len switched
 * to size_t so callers across modules can use it; table and loop logic
 * kept verbatim.
 *
 * Host unit test: pmu/test/host/uart_ipc_proto_host_test.c.
 */
#include "crc32.h"

uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    /* Polynomial 0xEDB88320 (reflected IEEE 802.3). */
    static const uint32_t crc_table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ crc_table[crc & 0x0F];
        crc = (crc >> 4) ^ crc_table[crc & 0x0F];
    }
    return ~crc;
}
