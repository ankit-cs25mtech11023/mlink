// SPDX-License-Identifier: GPL-2.0-only
/*
 * mlink - raw-IP network driver over an ivshmem-doorbell shared-memory link.
 *
 * Phase 0 bootstrap: proves the build -> stage -> insmod loop and that the
 * guest sees the ivshmem-doorbell device. No device is bound yet; the real
 * pci_driver arrives in Phase 1.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/pci.h>

#define MLINK_PCI_DEVICE_ID	0x1110	/* QEMU ivshmem (plain and doorbell) */

static int __init mlink_init(void)
{
	struct pci_dev *pdev = NULL;
	int n = 0;

	/* Each call drops the reference on the previous pdev and takes one on the next. */
	while ((pdev = pci_get_device(PCI_VENDOR_ID_REDHAT_QUMRANET,
				      MLINK_PCI_DEVICE_ID, pdev))) {
		resource_size_t regs = pci_resource_len(pdev, 0);
		resource_size_t msix = pci_resource_len(pdev, 1);
		resource_size_t shm = pci_resource_len(pdev, 2);

		pr_info("%s: BAR0 (regs) %pa, BAR1 (MSI-X) %pa, BAR2 (shm) %pa bytes\n",
			pci_name(pdev), &regs, &msix, &shm);
		n++;
	}

	pr_info("hello: %d ivshmem device(s)\n", n);
	return 0;
}

static void __exit mlink_exit(void)
{
	pr_info("goodbye\n");
}

module_init(mlink_init);
module_exit(mlink_exit);

MODULE_DESCRIPTION("Raw-IP network driver over an ivshmem shared-memory modem link");
MODULE_AUTHOR("Ankit <cs25mtech11023@iith.ac.in>");
MODULE_LICENSE("GPL");
