#ifndef ISML_DIAG_UAPI_H
#define ISML_DIAG_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ISML_DIAG_ABI_VERSION 1U
#define ISML_DIAG_IRQ_SELFTEST_VERSION 5U
#define ISML_DIAG_IOC_MAGIC 'I'

/* Maximum size of one coherent host DMA allocation request. */
#define ISML_DIAG_HOST_DMA_MAX_SIZE_BYTES (128ULL * 1024ULL * 1024ULL)

struct isml_diag_device_info {
	__u32 abi_version;
	__u16 domain;
	__u8 bus;
	__u8 devfn;
	__u32 dma_mask_bits;
	__u32 reserved;
};

struct isml_diag_dma_alloc {
	__u64 size;
	__u64 handle;
	__u64 device_addr;
	__u64 mmap_offset;
};

struct isml_diag_dma_free {
	__u64 handle;
};

/* Experimental MSI-X transport test; remove with the matching ioctl handler. */
struct isml_diag_irq_selftest {
	__u32 size;
	__u32 version;
	__u32 vector;
	__u32 flags;
	__u32 request_value;
	__u32 request_readback;
	__u32 request_after_clear;
	__s32 trigger_status;
	__u64 irq_count_before;
	__u64 irq_count_after;
	__u32 msix_addr_lo;
	__u32 msix_addr_hi;
	__u32 msix_data;
	__u32 msix_vector_control;
	__u32 message_addr_lo_readback;
	__u32 message_addr_hi_readback;
	__u32 message_data_readback;
	__u32 int_config5_before;
	__u32 int_config5_enabled;
	__u32 int_config0_before;
	__u32 int_config0_programmed;
	__u32 reserved0;
};

#define ISML_DIAG_IOCTL_GET_DEVICE_INFO \
	_IOR(ISML_DIAG_IOC_MAGIC, 0x00, struct isml_diag_device_info)
#define ISML_DIAG_IOCTL_DMA_ALLOC \
	_IOWR(ISML_DIAG_IOC_MAGIC, 0x01, struct isml_diag_dma_alloc)
#define ISML_DIAG_IOCTL_DMA_FREE \
	_IOW(ISML_DIAG_IOC_MAGIC, 0x02, struct isml_diag_dma_free)
#define ISML_DIAG_IOCTL_IRQ_SELFTEST \
	_IOWR(ISML_DIAG_IOC_MAGIC, 0x03, struct isml_diag_irq_selftest)

#endif
