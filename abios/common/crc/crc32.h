/* CRC-32/ISO-HDLC (IEEE 802.3, zlib compatible) – shared CRC helper.
 *
 * Same polynomial and result as zlib.crc32. Used by uart_ipc frame
 * checking and reusable by any other module (log check, flash, ...).
 *
 * Host unit test: pmu/test/host/uart_ipc_proto_host_test.c.
 */
#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

/* CRC-32/ISO-HDLC over data[0..len-1]. Zlib compatible:
 * crc32_ieee("123456789", 9) == 0xCBF43926. */
uint32_t crc32_ieee(const uint8_t *data, size_t len);

#endif /* CRC32_H */
