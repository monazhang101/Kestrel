// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "isml_diag_uapi.h"

int main(int argc, char **argv)
{
	struct isml_diag_irq_selftest test = {
		.size = sizeof(test),
		.version = ISML_DIAG_IRQ_SELFTEST_VERSION,
		.vector = 0,
	};
	int fd;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/isml_diagN\n", argv[0]);
		return 2;
	}

	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	if (ioctl(fd, ISML_DIAG_IOCTL_IRQ_SELFTEST, &test) != 0) {
		fprintf(stderr, "IRQ selftest ioctl: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	printf("vector=%u table=%08x:%08x data=%#x control=%#x status=%d "
	       "message_readback=%08x:%08x/%#x int_config5=%#x->%#x "
	       "int_config0=%#x->%#x "
	       "request=%#x readback=%#x "
	       "final=%#x irq_count=%" PRIu64
	       "->%" PRIu64 "\n",
	       test.vector, test.msix_addr_hi, test.msix_addr_lo,
	       test.msix_data, test.msix_vector_control, test.trigger_status,
	       test.message_addr_hi_readback, test.message_addr_lo_readback,
	       test.message_data_readback,
	       test.int_config5_before, test.int_config5_enabled,
	       test.int_config0_before, test.int_config0_programmed,
	       test.request_value, test.request_readback,
	       test.request_after_clear, (uint64_t)test.irq_count_before,
	       (uint64_t)test.irq_count_after);
	return 0;
}
