#ifndef ISML_DIAG_UAPI_H
#define ISML_DIAG_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ISML_DIAG_ABI_VERSION 1U
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

#define ISML_DIAG_IOCTL_GET_DEVICE_INFO \
	_IOR(ISML_DIAG_IOC_MAGIC, 0x00, struct isml_diag_device_info)
#define ISML_DIAG_IOCTL_DMA_ALLOC \
	_IOWR(ISML_DIAG_IOC_MAGIC, 0x01, struct isml_diag_dma_alloc)
#define ISML_DIAG_IOCTL_DMA_FREE \
	_IOW(ISML_DIAG_IOC_MAGIC, 0x02, struct isml_diag_dma_free)

#endif
