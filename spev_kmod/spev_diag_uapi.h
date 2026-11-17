#ifndef SPEV_DIAG_UAPI_H
#define SPEV_DIAG_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SPEV_DIAG_ABI_VERSION 1U
#define SPEV_DIAG_IOC_MAGIC 'I'

/* Maximum size of one coherent host DMA allocation request. */
#define SPEV_DIAG_HOST_DMA_MAX_SIZE_BYTES (128ULL * 1024ULL * 1024ULL)

struct spev_diag_device_info {
	__u32 abi_version;
	__u16 domain;
	__u8 bus;
	__u8 devfn;
	__u32 dma_mask_bits;
	__u32 reserved;
};

struct spev_diag_dma_alloc {
	__u64 size;
	__u64 handle;
	__u64 device_addr;
	__u64 mmap_offset;
};

struct spev_diag_dma_free {
	__u64 handle;
};

#define SPEV_DIAG_IOCTL_GET_DEVICE_INFO \
	_IOR(SPEV_DIAG_IOC_MAGIC, 0x00, struct spev_diag_device_info)
#define SPEV_DIAG_IOCTL_DMA_ALLOC \
	_IOWR(SPEV_DIAG_IOC_MAGIC, 0x01, struct spev_diag_dma_alloc)
#define SPEV_DIAG_IOCTL_DMA_FREE \
	_IOW(SPEV_DIAG_IOC_MAGIC, 0x02, struct spev_diag_dma_free)

#endif
