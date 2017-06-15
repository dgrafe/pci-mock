#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>

#ifdef __arch_um__
#include <mem_user.h>
#include <irq_kern.h>
#include <kern_util.h>
#include <os.h>
#endif

#define INTERRUPT_MASK_REGISTER	0x3C
#define INTERRUPT_STATUS_REGISTER 0x3E
#define LINK_CHANGE_INT_MASK	(1<<5)

#define PHY_STATUS_REGISTER	0x6C
#define LINK_STATUS_MASK	(1<<1)


static struct mock_private {
	void __iomem *mmio_addr;
	struct pci_dev *dev;
	int sock;
} PrivateData;

static void __exit demo_exit(void) {

	// disable interrupts
	iowrite16(0x0000, PrivateData.mmio_addr + INTERRUPT_MASK_REGISTER);

#ifndef __arch_um__
	free_irq(PrivateData.dev->irq, &PrivateData);
	
	pci_iounmap(PrivateData.dev, PrivateData.mmio_addr);
	pci_release_regions(PrivateData.dev);
#endif
}

static irqreturn_t int_handler(int irq, void *dev) {

	// was this interrupt generated because of a link change?
	if (LINK_CHANGE_INT_MASK == (LINK_CHANGE_INT_MASK & ioread16(PrivateData.mmio_addr + INTERRUPT_STATUS_REGISTER))) {


		// reset the status bit
		iowrite16(LINK_CHANGE_INT_MASK, PrivateData.mmio_addr + INTERRUPT_STATUS_REGISTER);

		// print the link change
		if (LINK_STATUS_MASK == (LINK_STATUS_MASK & ioread8(PrivateData.mmio_addr + PHY_STATUS_REGISTER)))
			printk(KERN_NOTICE "Link status changed to UP\n");
		else
			printk(KERN_NOTICE "Link status changed to DOWN\n");

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

//****************** UML specific initialisation ****************
#ifdef __arch_um__
static irqreturn_t int_handler_uml(int irq, void *dev) {

	int ret;

	// remove the trigger byte from the socket bffer
	os_read_file(*((int*)dev), &ret, sizeof(ret));

	// forward execution to the real handler
	ret = int_handler(irq, dev);
	
	// again, set the trigger
	reactivate_fd(*((int*)dev), TELNETD_IRQ);

	return ret;
}

static int init_device(void) {

	int rc;
	unsigned long iomem_length;
	char file[100];

	PrivateData.mmio_addr = (void*)find_iomem("mock", &iomem_length);
	if (!PrivateData.mmio_addr) {
		printk(KERN_ERR "UML: Could not map iomem\n");
		return -ENODEV;
	} else {
		printk(KERN_NOTICE "UML: iomem mapped with size %ld\n", iomem_length);

		if (umid_file_name("mock", file, sizeof(file)))
			return -1;

		PrivateData.sock = os_create_unix_socket(file, sizeof(file), 1);
		if (PrivateData.sock < 0) {
			printk(KERN_ERR "Failed to create IRQ trigger\n");
			return -ENODEV;
		}

		rc = um_request_irq(TELNETD_IRQ, PrivateData.sock, IRQ_READ, int_handler_uml,
					IRQF_SHARED, "mock-demo", &PrivateData.sock); 
		if (0 != rc) {

			printk(KERN_ERR "Failed to set handler on fake IRQ line. Err: %d\n", rc);
			return -ENODEV;
		}

	}

	return 0;
}

#else
//**************** PCI specific initalisation  ******************
static int init_device(void) {
	
	int rc;

	PrivateData.dev = pci_get_device(0x10ec, 0x8168, NULL);
	if (!PrivateData.dev) {
		printk(KERN_ERR "PCI Device not found");
		return -ENODEV;
	}

	rc = pci_enable_device(PrivateData.dev);
	if (rc < 0)
		goto err_enable;

	rc = pci_request_regions(PrivateData.dev, "mock-demo");
	if (rc != 0) {

		dev_err(&PrivateData.dev->dev, "Error requesting the pci regions: %d", rc);
		return -ENOMEM;
	}

	PrivateData.mmio_addr = pci_iomap(PrivateData.dev, 2, 0);
	if (!PrivateData.mmio_addr) {
		rc=-EIO;
		dev_err(&PrivateData.dev->dev, "Error mapping Bar");
		goto err_release;
	}

	// disable interrupts before requesting the handler
	iowrite16(0x0000, PrivateData.mmio_addr + INTERRUPT_MASK_REGISTER);

	rc = request_irq(PrivateData.dev->irq, int_handler, IRQF_SHARED, "mock-demo-handler", &PrivateData);
	if (0 != rc) {
		dev_err(&PrivateData.dev->dev, "Error rquesting interrupt: %d", rc);
		pci_iounmap(PrivateData.dev, PrivateData.mmio_addr);
		goto err_release;
	}

	return 0;

err_release:
	pci_release_regions(PrivateData.dev);
	return rc;

err_enable:
	dev_err(&PrivateData.dev->dev, "Error enabling device: %d\n", rc);
	return rc;
}
#endif
//***************************************************************

static int __init demo_init(void) {

	int rc;
	
	rc = init_device();
	if (0 != rc)
		return rc;

	// enable the link change interrupt after registering the handler
	iowrite16(LINK_CHANGE_INT_MASK, PrivateData.mmio_addr + INTERRUPT_MASK_REGISTER);

	return 0;
}

module_init(demo_init);
module_exit(demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Grafe (dgrafe@dgrafe.de");
MODULE_DESCRIPTION("Mocking PCI devices in a User Mode Linux environment");
