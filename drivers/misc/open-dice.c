// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 - Google LLC
 * Author: David Brazdil <dbrazdil@google.com>
 *
 * Driver for Open Profile for DICE.
 *
 * This driver takes ownership of a reserved memory region containing data
 * generated by the Open Profile for DICE measured boot protocol. The memory
 * contents are not interpreted by the kernel but can be mapped into a userspace
 * process via a misc device. Userspace can also request a wipe of the memory.
 *
 * Userspace can access the data with (w/o error handling):
 *
 *     fd = open("/dev/open-dice0", O_RDWR);
 *     read(fd, &size, sizeof(unsigned long));
 *     data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
 *     write(fd, NULL, 0); // wipe
 *     close(fd);
 */

#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#define DRIVER_NAME "open-dice"

struct open_dice_drvdata {
	struct mutex lock;
	char name[16];
	struct reserved_mem *rmem;
	struct miscdevice misc;
};

static inline struct open_dice_drvdata *to_open_dice_drvdata(struct file *filp)
{
	return container_of(filp->private_data, struct open_dice_drvdata, misc);
}

static int open_dice_wipe(struct open_dice_drvdata *drvdata)
{
	void *kaddr;

	mutex_lock(&drvdata->lock);
	kaddr = devm_memremap(drvdata->misc.this_device, drvdata->rmem->base,
			      drvdata->rmem->size, MEMREMAP_WC);
	if (IS_ERR(kaddr)) {
		mutex_unlock(&drvdata->lock);
		return PTR_ERR(kaddr);
	}

	memset(kaddr, 0, drvdata->rmem->size);
	devm_memunmap(drvdata->misc.this_device, kaddr);
	mutex_unlock(&drvdata->lock);
	return 0;
}

/*
 * Copies the size of the reserved memory region to the user-provided buffer.
 */
static ssize_t open_dice_read(struct file *filp, char __user *ptr, size_t len,
			      loff_t *off)
{
	unsigned long val = to_open_dice_drvdata(filp)->rmem->size;

	return simple_read_from_buffer(ptr, len, off, &val, sizeof(val));
}

/*
 * Triggers a wipe of the reserved memory region. The user-provided pointer
 * is never dereferenced.
 */
static ssize_t open_dice_write(struct file *filp, const char __user *ptr,
			       size_t len, loff_t *off)
{
	if (open_dice_wipe(to_open_dice_drvdata(filp)))
		return -EIO;

	/* Consume the input buffer. */
	return len;
}

/*
 * Creates a mapping of the reserved memory region in user address space.
 */
static int open_dice_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct open_dice_drvdata *drvdata = to_open_dice_drvdata(filp);

	/* Do not allow userspace to modify the underlying data. */
	if ((vma->vm_flags & VM_WRITE) && (vma->vm_flags & VM_SHARED))
		return -EPERM;

	/* Ensure userspace cannot acquire VM_WRITE + VM_SHARED later. */
	if (vma->vm_flags & VM_WRITE)
		vm_flags_clear(vma, VM_MAYSHARE);
	else if (vma->vm_flags & VM_SHARED)
		vm_flags_clear(vma, VM_MAYWRITE);

	/* Create write-combine mapping so all clients observe a wipe. */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTDUMP);
	return vm_iomap_memory(vma, drvdata->rmem->base, drvdata->rmem->size);
}

static const struct file_operations open_dice_fops = {
	.owner = THIS_MODULE,
	.read = open_dice_read,
	.write = open_dice_write,
	.mmap = open_dice_mmap,
};

static int __init open_dice_probe(struct platform_device *pdev)
{
	static unsigned int dev_idx;
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem;
	struct open_dice_drvdata *drvdata;
	int ret;

	rmem = of_reserved_mem_lookup(dev->of_node);
	if (!rmem) {
		dev_err(dev, "failed to lookup reserved memory\n");
		return -EINVAL;
	}

	if (!rmem->size || (rmem->size > ULONG_MAX)) {
		dev_err(dev, "invalid memory region size\n");
		return -EINVAL;
	}

	if (!PAGE_ALIGNED(rmem->base) || !PAGE_ALIGNED(rmem->size)) {
		dev_err(dev, "memory region must be page-aligned\n");
		return -EINVAL;
	}

	drvdata = devm_kmalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	*drvdata = (struct open_dice_drvdata){
		.rmem = rmem,
		.misc = (struct miscdevice){
			.parent	= dev,
			.name	= drvdata->name,
			.minor	= MISC_DYNAMIC_MINOR,
			.fops	= &open_dice_fops,
			.mode	= 0600,
		},
	};
	mutex_init(&drvdata->lock);

	/* Index overflow check not needed, misc_register() will fail. */
	snprintf(drvdata->name, sizeof(drvdata->name), DRIVER_NAME"%u", dev_idx++);

	ret = misc_register(&drvdata->misc);
	if (ret) {
		dev_err(dev, "failed to register misc device '%s': %d\n",
			drvdata->name, ret);
		return ret;
	}

	platform_set_drvdata(pdev, drvdata);
	return 0;
}

static int open_dice_remove(struct platform_device *pdev)
{
	struct open_dice_drvdata *drvdata = platform_get_drvdata(pdev);

	misc_deregister(&drvdata->misc);
	return 0;
}

static const struct of_device_id open_dice_of_match[] = {
	{ .compatible = "google,open-dice" },
	{},
};

static struct platform_driver open_dice_driver = {
	.remove = open_dice_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = open_dice_of_match,
	},
};

static int __init open_dice_init(void)
{
	int ret = platform_driver_probe(&open_dice_driver, open_dice_probe);

	/* DICE regions are optional. Succeed even with zero instances. */
	return (ret == -ENODEV) ? 0 : ret;
}

static void __exit open_dice_exit(void)
{
	platform_driver_unregister(&open_dice_driver);
}

module_init(open_dice_init);
module_exit(open_dice_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("David Brazdil <dbrazdil@google.com>");