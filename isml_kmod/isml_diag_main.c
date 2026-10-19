// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
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

/*
 * Experimental MSI-X request register in the host BAR0 view. Atlas maps the
 * internal PCIe CSR base 0x16000000 at BAR0 offset 0x36000000.
 * Remove these definitions with isml_irq_selftest().
 */
#define ISML_IRQ_SELFTEST_BAR 0
#define ISML_IRQ_SELFTEST_OFFSET 0x36010228ULL
#define ISML_IRQ_SELFTEST_REG_SIZE 0x34
#define ISML_IRQ_SELFTEST_REQ BIT(0)
#define ISML_IRQ_SELFTEST_VECTOR_SHIFT 7
#define ISML_IRQ_SELFTEST_VECTOR_MASK GENMASK(11, 7)

/* lspci reports the MSI-X table in BAR0 at offset 0, 16 bytes per entry. */
#define ISML_MSIX_TABLE_BAR 0
#define ISML_MSIX_TABLE_OFFSET 0ULL
#define ISML_MSIX_ENTRY_SIZE (4 * sizeof(u32))
#define ISML_MSIX_ADDR_LO_OFFSET 0
#define ISML_MSIX_ADDR_HI_OFFSET 4
#define ISML_MSIX_DATA_OFFSET 8
#define ISML_MSIX_VECTOR_CONTROL_OFFSET 12
#define ISML_MSIX_VECTOR_MASKED BIT(0)

#define ISML_MSI_MESSAGE_DATA_OFFSET 4
#define ISML_MSI_MESSAGE_ADDR_LO_OFFSET 8
#define ISML_MSI_MESSAGE_ADDR_HI_OFFSET 12

#define ISML_INT_CONFIG0_OFFSET 0x2c
#define ISML_INT_CONFIG5_OFFSET 0x30
#define ISML_INT_CONFIG5_ENABLE BIT(4)

struct isml_device {
	struct pci_dev *pdev;
	struct miscdevice miscdev;
	char name[24];
	int instance;
	u32 dma_mask_bits;
	int irq;
	int irq_vectors;
	atomic64_t irq_count;
	struct mutex irq_test_lock;
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

static irqreturn_t isml_irq_handler(int irq, void *data)
{
	struct isml_device *isml = data;
	s64 count = atomic64_inc_return(&isml->irq_count);

	/* Phase-1 smoke path: this is a dedicated MSI-X vector. */
	dev_info_ratelimited(&isml->pdev->dev,
			     "MSI-X interrupt: irq=%d count=%lld\n",
			     irq, (long long)count);
	return IRQ_HANDLED;
}

static int isml_irq_selftest(struct isml_device *isml,
			     struct isml_diag_irq_selftest *test)
{
	struct pci_dev *pdev = isml->pdev;
	u8 __iomem *message_regs;
	u8 __iomem *table_entry;
	resource_size_t bar_size;
	u32 request_value;

	if (test->vector >= isml->irq_vectors || test->flags)
		return -EINVAL;
	if (!(pci_resource_flags(pdev, ISML_IRQ_SELFTEST_BAR) & IORESOURCE_MEM))
		return -ENODEV;

	bar_size = pci_resource_len(pdev, ISML_IRQ_SELFTEST_BAR);
	if (ISML_IRQ_SELFTEST_OFFSET > bar_size ||
	    bar_size - ISML_IRQ_SELFTEST_OFFSET < ISML_IRQ_SELFTEST_REG_SIZE ||
	    ISML_MSIX_TABLE_OFFSET > bar_size ||
	    bar_size - ISML_MSIX_TABLE_OFFSET < ISML_MSIX_ENTRY_SIZE)
		return -ERANGE;

	message_regs = pci_iomap_range(pdev, ISML_IRQ_SELFTEST_BAR,
					ISML_IRQ_SELFTEST_OFFSET,
					ISML_IRQ_SELFTEST_REG_SIZE);
	if (!message_regs)
		return -ENOMEM;
	table_entry = pci_iomap_range(pdev, ISML_MSIX_TABLE_BAR,
				      ISML_MSIX_TABLE_OFFSET,
				      ISML_MSIX_ENTRY_SIZE);
	if (!table_entry) {
		pci_iounmap(pdev, message_regs);
		return -ENOMEM;
	}

	request_value = ISML_IRQ_SELFTEST_REQ |
		(test->vector << ISML_IRQ_SELFTEST_VECTOR_SHIFT);
	request_value &= ISML_IRQ_SELFTEST_REQ | ISML_IRQ_SELFTEST_VECTOR_MASK;

	mutex_lock(&isml->irq_test_lock);
	test->msix_addr_lo = readl(table_entry + ISML_MSIX_ADDR_LO_OFFSET);
	test->msix_addr_hi = readl(table_entry + ISML_MSIX_ADDR_HI_OFFSET);
	test->msix_data = readl(table_entry + ISML_MSIX_DATA_OFFSET);
	test->msix_vector_control =
		readl(table_entry + ISML_MSIX_VECTOR_CONTROL_OFFSET);
	test->trigger_status = 0;
	if (!test->msix_addr_lo && !test->msix_addr_hi) {
		test->trigger_status = -ENXIO;
		goto unlock;
	}
	if (test->msix_vector_control & ISML_MSIX_VECTOR_MASKED) {
		test->trigger_status = -EAGAIN;
		goto unlock;
	}

	test->int_config5_before =
		readl(message_regs + ISML_INT_CONFIG5_OFFSET);
	writel(test->int_config5_before | ISML_INT_CONFIG5_ENABLE,
	       message_regs + ISML_INT_CONFIG5_OFFSET);
	test->int_config5_enabled =
		readl(message_regs + ISML_INT_CONFIG5_OFFSET);
	if (!(test->int_config5_enabled & ISML_INT_CONFIG5_ENABLE)) {
		test->trigger_status = -EIO;
		goto restore_int_config5;
	}

	test->int_config0_before =
		readl(message_regs + ISML_INT_CONFIG0_OFFSET);
	writel(test->msix_addr_lo,
	       message_regs + ISML_INT_CONFIG0_OFFSET);
	test->int_config0_programmed =
		readl(message_regs + ISML_INT_CONFIG0_OFFSET);

	/* Mode B: copy Linux's MSI-X table entry into the device message CSRs. */
	writel(test->msix_data,
	       message_regs + ISML_MSI_MESSAGE_DATA_OFFSET);
	writel(test->msix_addr_lo,
	       message_regs + ISML_MSI_MESSAGE_ADDR_LO_OFFSET);
	writel(test->msix_addr_hi,
	       message_regs + ISML_MSI_MESSAGE_ADDR_HI_OFFSET);
	test->message_data_readback =
		readl(message_regs + ISML_MSI_MESSAGE_DATA_OFFSET);
	test->message_addr_lo_readback =
		readl(message_regs + ISML_MSI_MESSAGE_ADDR_LO_OFFSET);
	test->message_addr_hi_readback =
		readl(message_regs + ISML_MSI_MESSAGE_ADDR_HI_OFFSET);

	test->irq_count_before = atomic64_read(&isml->irq_count);
	test->request_value = request_value;
	writel(request_value, message_regs);
	readl(message_regs); /* Flush the posted write before observing self-clear. */
	usleep_range(10000, 12000);
	test->request_readback = readl(message_regs);

	if (test->request_readback & ISML_IRQ_SELFTEST_REQ) {
		writel(request_value & ~ISML_IRQ_SELFTEST_REQ, message_regs);
		readl(message_regs);
	}
	test->request_after_clear = readl(message_regs);
	test->irq_count_after = atomic64_read(&isml->irq_count);

	writel(test->int_config0_before,
	       message_regs + ISML_INT_CONFIG0_OFFSET);
	readl(message_regs + ISML_INT_CONFIG0_OFFSET);

restore_int_config5:
	writel(test->int_config5_before,
	       message_regs + ISML_INT_CONFIG5_OFFSET);
	readl(message_regs + ISML_INT_CONFIG5_OFFSET);

unlock:
	mutex_unlock(&isml->irq_test_lock);

	pci_iounmap(pdev, table_entry);
	pci_iounmap(pdev, message_regs);
	dev_info(&pdev->dev,
		 "MSI-X selftest: vector=%u table=%08x:%08x data=%#x control=%#x message_readback=%08x:%08x/%#x int_config5=%#x->%#x int_config0=%#x->%#x status=%d request=%#x readback=%#x final=%#x irq_count=%llu->%llu\n",
		 test->vector, test->msix_addr_hi, test->msix_addr_lo,
		 test->msix_data, test->msix_vector_control,
		 test->message_addr_hi_readback,
		 test->message_addr_lo_readback,
		 test->message_data_readback,
		 test->int_config5_before, test->int_config5_enabled,
		 test->int_config0_before, test->int_config0_programmed,
		 test->trigger_status, test->request_value, test->request_readback,
		 test->request_after_clear, test->irq_count_before,
		 test->irq_count_after);
	return 0;
}

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
	case ISML_DIAG_IOCTL_IRQ_SELFTEST: {
		struct isml_diag_irq_selftest test;
		int ret;

		if (!capable(CAP_SYS_RAWIO))
			return -EPERM;
		if (copy_from_user(&test, user_arg, sizeof(test)))
			return -EFAULT;
		if (test.size != sizeof(test) ||
		    test.version != ISML_DIAG_IRQ_SELFTEST_VERSION)
			return -EINVAL;

		ret = isml_irq_selftest(ctx->isml, &test);
		if (ret)
			return ret;
		return copy_to_user(user_arg, &test, sizeof(test)) ? -EFAULT : 0;
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
	atomic64_set(&isml->irq_count, 0);
	mutex_init(&isml->irq_test_lock);

	/* Experiment 1: require exactly one MSI-X vector; do not fall back to INTx. */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
	if (ret < 0)
		goto disable_device;
	isml->irq_vectors = ret;
	isml->irq = pci_irq_vector(pdev, 0);
	if (isml->irq < 0) {
		ret = isml->irq;
		goto free_irq_vectors;
	}

	ret = request_irq(isml->irq, isml_irq_handler, 0, "isml_diag", isml);
	if (ret)
		goto free_irq_vectors;

	instance = ida_alloc(&isml_device_ids, GFP_KERNEL);
	if (instance < 0) {
		ret = instance;
		goto free_irq;
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
		goto free_irq;
	}

	pci_set_drvdata(pdev, isml);
	dev_info(&pdev->dev,
		 "%s ready, DMA mask %u-bit, MSI-X vectors %d, irq %d\n",
		 isml->name, isml->dma_mask_bits,
		 isml->irq_vectors, isml->irq);
	return 0;

free_irq:
	free_irq(isml->irq, isml);
free_irq_vectors:
	pci_free_irq_vectors(pdev);
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
	free_irq(isml->irq, isml);
	pci_free_irq_vectors(pdev);
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
