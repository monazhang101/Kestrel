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

#include "isml_diag_uapi.h"

/* Current Atlas bring-up PCI IDs; keep synchronized with the platform ID table. */
#define ISML_VENDOR_ID 0x16c3
#define ISML_DEVICE_ID 0xabcd

struct isml_device {
	struct pci_dev *pdev;
	struct miscdevice miscdev;
	char name[24];
	int instance;
	u32 dma_mask_bits;
};

struct isml_dma_buffer {
	struct list_head node;
	struct kref ref;
	struct device *dev;
	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;
	u64 handle;
};

struct isml_file {
	struct isml_device *isml;
	struct mutex lock;
	struct list_head buffers;
	u64 next_handle;
};

static DEFINE_IDA(isml_device_ids);

static void isml_dma_buffer_release(struct kref *ref)
{
	struct isml_dma_buffer *buffer =
		container_of(ref, struct isml_dma_buffer, ref);

	dma_free_coherent(buffer->dev, buffer->size,
			  buffer->cpu_addr, buffer->dma_addr);
	kfree(buffer);
}

static struct isml_dma_buffer *isml_find_buffer(struct isml_file *ctx,
						 u64 handle)
{
	struct isml_dma_buffer *buffer;

	list_for_each_entry(buffer, &ctx->buffers, node) {
		if (buffer->handle == handle)
			return buffer;
	}
	return NULL;
}

static int isml_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct isml_device *isml =
		container_of(misc, struct isml_device, miscdev);
	struct isml_file *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->isml = isml;
	ctx->next_handle = 1;
	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->buffers);
	file->private_data = ctx;
	return 0;
}

static int isml_release(struct inode *inode, struct file *file)
{
	struct isml_file *ctx = file->private_data;
	struct isml_dma_buffer *buffer, *tmp;

	mutex_lock(&ctx->lock);
	list_for_each_entry_safe(buffer, tmp, &ctx->buffers, node) {
		list_del(&buffer->node);
		kref_put(&buffer->ref, isml_dma_buffer_release);
	}
	mutex_unlock(&ctx->lock);
	kfree(ctx);
	return 0;
}

static long isml_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct isml_file *ctx = file->private_data;
	void __user *user_arg = (void __user *)arg;

	if (_IOC_TYPE(cmd) != ISML_DIAG_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case ISML_DIAG_IOCTL_GET_DEVICE_INFO: {
		struct pci_dev *pdev = ctx->isml->pdev;
		struct isml_diag_device_info info = {
			.abi_version = ISML_DIAG_ABI_VERSION,
			.domain = pci_domain_nr(pdev->bus),
			.bus = pdev->bus->number,
			.devfn = pdev->devfn,
			.dma_mask_bits = ctx->isml->dma_mask_bits,
		};

		return copy_to_user(user_arg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case ISML_DIAG_IOCTL_DMA_ALLOC: {
		struct isml_diag_dma_alloc request;
		struct isml_dma_buffer *buffer;
		size_t size;

		if (copy_from_user(&request, user_arg, sizeof(request)))
			return -EFAULT;
		if (!request.size ||
		    request.size > ISML_DIAG_HOST_DMA_MAX_SIZE_BYTES)
			return -EINVAL;

		size = PAGE_ALIGN(request.size);
		buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
		if (!buffer)
			return -ENOMEM;

		buffer->dev = &ctx->isml->pdev->dev;
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
			kref_put(&buffer->ref, isml_dma_buffer_release);
			return -EFAULT;
		}
		return 0;
	}
	case ISML_DIAG_IOCTL_DMA_FREE: {
		struct isml_diag_dma_free request;
		struct isml_dma_buffer *buffer;

		if (copy_from_user(&request, user_arg, sizeof(request)))
			return -EFAULT;

		mutex_lock(&ctx->lock);
		buffer = isml_find_buffer(ctx, request.handle);
		if (buffer)
			list_del(&buffer->node);
		mutex_unlock(&ctx->lock);
		if (!buffer)
			return -ENOENT;

		kref_put(&buffer->ref, isml_dma_buffer_release);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static void isml_vma_open(struct vm_area_struct *vma)
{
	struct isml_dma_buffer *buffer = vma->vm_private_data;

	kref_get(&buffer->ref);
}

static void isml_vma_close(struct vm_area_struct *vma)
{
	struct isml_dma_buffer *buffer = vma->vm_private_data;

	kref_put(&buffer->ref, isml_dma_buffer_release);
}

static const struct vm_operations_struct isml_vm_ops = {
	.open = isml_vma_open,
	.close = isml_vma_close,
};

static int isml_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct isml_file *ctx = file->private_data;
	struct isml_dma_buffer *buffer;
	u64 handle = vma->vm_pgoff;
	unsigned long requested_size = vma->vm_end - vma->vm_start;
	int ret;

	mutex_lock(&ctx->lock);
	buffer = isml_find_buffer(ctx, handle);
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

	vma->vm_ops = &isml_vm_ops;
	vma->vm_private_data = buffer;
	return 0;

put_buffer:
	kref_put(&buffer->ref, isml_dma_buffer_release);
	return ret;
}

static const struct file_operations isml_fops = {
	.owner = THIS_MODULE,
	.open = isml_open,
	.release = isml_release,
	.unlocked_ioctl = isml_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = isml_ioctl,
#endif
	.mmap = isml_mmap,
	/* Leave .llseek NULL: this ioctl/mmap-only device is not seekable. */
};

static int isml_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct isml_device *isml;
	int instance;
	int ret;

	isml = devm_kzalloc(&pdev->dev, sizeof(*isml), GFP_KERNEL);
	if (!isml)
		return -ENOMEM;
	isml->pdev = pdev;

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

	isml->dma_mask_bits = dma_get_mask(&pdev->dev) > DMA_BIT_MASK(32) ? 64 : 32;

	instance = ida_alloc(&isml_device_ids, GFP_KERNEL);
	if (instance < 0) {
		ret = instance;
		goto disable_device;
	}
	isml->instance = instance;
	snprintf(isml->name, sizeof(isml->name), "isml_diag%d", instance);
	isml->miscdev.minor = MISC_DYNAMIC_MINOR;
	isml->miscdev.name = isml->name;
	isml->miscdev.fops = &isml_fops;
	isml->miscdev.parent = &pdev->dev;

	ret = misc_register(&isml->miscdev);
	if (ret) {
		ida_free(&isml_device_ids, instance);
		goto disable_device;
	}

	pci_set_drvdata(pdev, isml);
	dev_info(&pdev->dev, "%s ready, DMA mask %u-bit\n",
		 isml->name, isml->dma_mask_bits);
	return 0;

disable_device:
	pci_clear_master(pdev);
	pci_disable_device(pdev);
	return ret;
}

static void isml_remove(struct pci_dev *pdev)
{
	struct isml_device *isml = pci_get_drvdata(pdev);
	misc_deregister(&isml->miscdev);
	ida_free(&isml_device_ids, isml->instance);
	pci_clear_master(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id isml_pci_ids[] = {
	{ PCI_DEVICE(ISML_VENDOR_ID, ISML_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, isml_pci_ids);

static struct pci_driver isml_pci_driver = {
	.name = "isml_diag",
	.id_table = isml_pci_ids,
	.probe = isml_probe,
	.remove = isml_remove,
};
module_pci_driver(isml_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ISML diagnostic DMA memory driver");
MODULE_AUTHOR("ISML");
