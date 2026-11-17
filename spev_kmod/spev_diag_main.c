// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "spev_diag_uapi.h"

/* Current Atlas bring-up PCI IDs; keep synchronized with the platform ID table. */
#define SPEV_VENDOR_ID 0x16c3
#define SPEV_DEVICE_ID 0xabcd
#define SPEV_DRIVER_NAME "spev_drv"

struct spev_device {
	struct pci_dev *pdev;
	struct miscdevice miscdev;
	char name[24];
	int instance;
	u32 dma_mask_bits;
};

struct spev_dma_buffer {
	struct list_head node;
	struct kref ref;
	struct device *dev;
	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;
	u64 handle;
};

struct spev_file {
	struct spev_device *spev;
	struct mutex lock;
	struct list_head buffers;
	u64 next_handle;
};

static DEFINE_IDA(spev_device_ids);

static void spev_dma_buffer_release(struct kref *ref)
{
	struct spev_dma_buffer *buffer =
		container_of(ref, struct spev_dma_buffer, ref);

	dma_free_coherent(buffer->dev, buffer->size,
			  buffer->cpu_addr, buffer->dma_addr);
	kfree(buffer);
}

static struct spev_dma_buffer *spev_find_buffer(struct spev_file *ctx,
						 u64 handle)
{
	struct spev_dma_buffer *buffer;

	list_for_each_entry(buffer, &ctx->buffers, node) {
		if (buffer->handle == handle)
			return buffer;
	}
	return NULL;
}

static int spev_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct spev_device *spev =
		container_of(misc, struct spev_device, miscdev);
	struct spev_file *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->spev = spev;
	ctx->next_handle = 1;
	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->buffers);
	file->private_data = ctx;
	return 0;
}

static int spev_release(struct inode *inode, struct file *file)
{
	struct spev_file *ctx = file->private_data;
	struct spev_dma_buffer *buffer, *tmp;

	mutex_lock(&ctx->lock);
	list_for_each_entry_safe(buffer, tmp, &ctx->buffers, node) {
		list_del(&buffer->node);
		kref_put(&buffer->ref, spev_dma_buffer_release);
	}
	mutex_unlock(&ctx->lock);
	kfree(ctx);
	return 0;
}

static long spev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct spev_file *ctx = file->private_data;
	void __user *user_arg = (void __user *)arg;

	if (_IOC_TYPE(cmd) != SPEV_DIAG_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case SPEV_DIAG_IOCTL_GET_DEVICE_INFO: {
		struct pci_dev *pdev = ctx->spev->pdev;
		struct spev_diag_device_info info = {
			.abi_version = SPEV_DIAG_ABI_VERSION,
			.domain = pci_domain_nr(pdev->bus),
			.bus = pdev->bus->number,
			.devfn = pdev->devfn,
			.dma_mask_bits = ctx->spev->dma_mask_bits,
		};

		return copy_to_user(user_arg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case SPEV_DIAG_IOCTL_DMA_ALLOC: {
		struct spev_diag_dma_alloc request;
		struct spev_dma_buffer *buffer;
		size_t size;

		if (copy_from_user(&request, user_arg, sizeof(request)))
			return -EFAULT;
		if (!request.size ||
		    request.size > SPEV_DIAG_HOST_DMA_MAX_SIZE_BYTES)
			return -EINVAL;

		size = PAGE_ALIGN(request.size);
		buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
		if (!buffer)
			return -ENOMEM;

		buffer->dev = &ctx->spev->pdev->dev;
		buffer->size = size;
		buffer->cpu_addr = dma_alloc_coherent(buffer->dev, size,
						      &buffer->dma_addr, GFP_KERNEL);
		if (!buffer->cpu_addr) {
			kfree(buffer);
			return -ENOMEM;
		}

		kref_init(&buffer->ref);
		mutex_lock(&ctx->lock);
		buffer->handle = ctx->next_handle++;
		list_add_tail(&buffer->node, &ctx->buffers);
		mutex_unlock(&ctx->lock);

		request.size = size;
		request.handle = buffer->handle;
		request.device_addr = buffer->dma_addr;
		request.mmap_offset = buffer->handle << PAGE_SHIFT;
		if (copy_to_user(user_arg, &request, sizeof(request))) {
			mutex_lock(&ctx->lock);
			list_del(&buffer->node);
			mutex_unlock(&ctx->lock);
			kref_put(&buffer->ref, spev_dma_buffer_release);
			return -EFAULT;
		}
		return 0;
	}
	case SPEV_DIAG_IOCTL_DMA_FREE: {
		struct spev_diag_dma_free request;
		struct spev_dma_buffer *buffer;

		if (copy_from_user(&request, user_arg, sizeof(request)))
			return -EFAULT;

		mutex_lock(&ctx->lock);
		buffer = spev_find_buffer(ctx, request.handle);
		if (buffer)
			list_del(&buffer->node);
		mutex_unlock(&ctx->lock);
		if (!buffer)
			return -ENOENT;

		kref_put(&buffer->ref, spev_dma_buffer_release);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static void spev_vma_open(struct vm_area_struct *vma)
{
	struct spev_dma_buffer *buffer = vma->vm_private_data;

	kref_get(&buffer->ref);
}

static void spev_vma_close(struct vm_area_struct *vma)
{
	struct spev_dma_buffer *buffer = vma->vm_private_data;

	kref_put(&buffer->ref, spev_dma_buffer_release);
}

static const struct vm_operations_struct spev_vm_ops = {
	.open = spev_vma_open,
	.close = spev_vma_close,
};

static int spev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct spev_file *ctx = file->private_data;
	struct spev_dma_buffer *buffer;
	u64 handle = vma->vm_pgoff;
	unsigned long requested_size = vma->vm_end - vma->vm_start;
	int ret;

	mutex_lock(&ctx->lock);
	buffer = spev_find_buffer(ctx, handle);
	if (buffer)
		kref_get(&buffer->ref);
	mutex_unlock(&ctx->lock);
	if (!buffer)
		return -ENOENT;
	if (requested_size != buffer->size) {
		ret = -EINVAL;
		goto put_buffer;
	}

	vma->vm_pgoff = 0;
	ret = dma_mmap_coherent(buffer->dev, vma, buffer->cpu_addr,
				buffer->dma_addr, buffer->size);
	if (ret)
		goto put_buffer;

	vma->vm_ops = &spev_vm_ops;
	vma->vm_private_data = buffer;
	return 0;

put_buffer:
	kref_put(&buffer->ref, spev_dma_buffer_release);
	return ret;
}

static const struct file_operations spev_fops = {
	.owner = THIS_MODULE,
	.open = spev_open,
	.release = spev_release,
	.unlocked_ioctl = spev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = spev_ioctl,
#endif
	.mmap = spev_mmap,
	/* Leave .llseek NULL: this ioctl/mmap-only device is not seekable. */
};

static int spev_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct spev_device *spev;
	int instance;
	int ret;

	spev = devm_kzalloc(&pdev->dev, sizeof(*spev), GFP_KERNEL);
	if (!spev)
		return -ENOMEM;
	spev->pdev = pdev;

	ret = pci_enable_device_mem(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);
	if (!dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64))) {
		/* Descriptor ABI carries the DMA address as low/high 32-bit words. */
		ret = 0;
	} else {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret)
			goto disable_device;
	}

	spev->dma_mask_bits = dma_get_mask(&pdev->dev) > DMA_BIT_MASK(32) ? 64 : 32;

	instance = ida_alloc(&spev_device_ids, GFP_KERNEL);
	if (instance < 0) {
		ret = instance;
		goto disable_device;
	}
	spev->instance = instance;
	snprintf(spev->name, sizeof(spev->name), "%s%d",
		 SPEV_DRIVER_NAME, instance);
	spev->miscdev.minor = MISC_DYNAMIC_MINOR;
	spev->miscdev.name = spev->name;
	spev->miscdev.fops = &spev_fops;
	spev->miscdev.parent = &pdev->dev;

	ret = misc_register(&spev->miscdev);
	if (ret) {
		ida_free(&spev_device_ids, instance);
		goto disable_device;
	}

	pci_set_drvdata(pdev, spev);
	dev_info(&pdev->dev, "%s ready, DMA mask %u-bit\n",
		 spev->name, spev->dma_mask_bits);
	return 0;

disable_device:
	pci_clear_master(pdev);
	pci_disable_device(pdev);
	return ret;
}

static void spev_remove(struct pci_dev *pdev)
{
	struct spev_device *spev = pci_get_drvdata(pdev);
	misc_deregister(&spev->miscdev);
	ida_free(&spev_device_ids, spev->instance);
	pci_clear_master(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id spev_pci_ids[] = {
	{ PCI_DEVICE(SPEV_VENDOR_ID, SPEV_DEVICE_ID) },
	{ PCI_DEVICE(0x213e, 0x0000) },
	{ }
};
MODULE_DEVICE_TABLE(pci, spev_pci_ids);

static struct pci_driver spev_pci_driver = {
	.name = SPEV_DRIVER_NAME,
	.id_table = spev_pci_ids,
	.probe = spev_probe,
	.remove = spev_remove,
};
module_pci_driver(spev_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SPEV DMA memory driver");
MODULE_AUTHOR("SPEV");
