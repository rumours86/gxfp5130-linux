// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental GXFP51B7 transport, Huawei MACHC-WAX9 with BIOS 1.26 only.
 *
 * TX: a plain MP frame at mailbox +0, then the EC is notified by writing
 *     0xf0 to its CMDR register (bounded wait, AML mutex held).
 * RX: a contiguous MP frame at mailbox +0x1000, signalled by an active-low
 *     IRQ and acknowledged through the host-ready GPIO.
 * No reset, provisioning or sensor configuration happens here; userspace
 * drives the sensor.
 */
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/iommu.h>
#include <linux/pci.h>
#include <linux/unaligned.h>
#include "gxfp_priv.h"
#include "gxfp_51b7.h"
#include "../driver/gxfp_uapi.h"
#include "../proto/gxfp_mp_proto.h"

#define RX_OFF 0x1000
#define EC_PATH "\\_SB.PCI0.LPCB.HWEC"
#define CMDR 0x3a

static int gxfp_51b7_check_dma(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct pci_dev *lpc = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(31, 0));
	struct iommu_domain *domain;
	resource_size_t addr;
	int ret = 0;

	if (!lpc || !res) {
		pci_dev_put(lpc);
		return -ENODEV;
	}
	/*
	 * The EC writes RX frames into the mailbox through the LPC bridge
	 * (00:1f.0). The firmware has no RMRR for this range, so under a DMA
	 * domain those writes fault. Refuse traffic unless the range is
	 * identity-mapped: an RMRR added by a DMAR override, or iommu=pt
	 * (docs/gxfp51b7.md).
	 */
	domain = iommu_get_domain_for_dev(&lpc->dev);
	if (domain && domain->type != IOMMU_DOMAIN_IDENTITY) {
		for (addr = res->start; addr <= res->end; addr += PAGE_SIZE) {
			if (iommu_iova_to_phys(domain, addr) != addr) {
				dev_err(&pdev->dev, "51B7 mailbox missing identity DMA mapping; refusing traffic\n");
				ret = -EACCES;
				break;
			}
		}
	}
	pci_dev_put(lpc);
	return ret;
}

int gxfp_51b7_init_gpios(struct platform_device *pdev)
{
	struct gxfp_dev *g = platform_get_drvdata(pdev);
	acpi_handle ec;
	int ret;

	/* EC register and GPIO protocol are verified only for this firmware. */
	if (!dmi_match(DMI_PRODUCT_NAME, "MACHC-WAX9") ||
	    !dmi_match(DMI_BIOS_VERSION, "1.26")) {
		dev_info(&pdev->dev, "51B7: only MACHC-WAX9 BIOS 1.26 is verified, not binding\n");
		return -ENODEV;
	}
	if (ACPI_FAILURE(acpi_get_handle(NULL, EC_PATH, &ec))) {
		dev_err(&pdev->dev, "51B7: %s not found\n", EC_PATH);
		return -ENODEV;
	}
	ret = gxfp_51b7_check_dma(pdev);
	if (ret)
		return ret;
	g->hw.gpio_irq = devm_gpiod_get_index(&pdev->dev, NULL, 0, GPIOD_IN);
	if (IS_ERR(g->hw.gpio_irq))
		return PTR_ERR(g->hw.gpio_irq);
	g->hw.gpio_read_done = devm_gpiod_get_index(&pdev->dev, NULL, 1, GPIOD_ASIS);
	if (IS_ERR(g->hw.gpio_read_done))
		return PTR_ERR(g->hw.gpio_read_done);
	/* EC bb72c waits for host=1 before DMA, then host=0 after IRQ. */
	ret = gpiod_direction_output_raw(g->hw.gpio_read_done, 1);
	if (ret)
		return ret;
	dev_info(&pdev->dev, "51B7: direct MP TX / RX+0x1000; IRQ raw=%d, host ready=1\n",
		 gpiod_get_raw_value_cansleep(g->hw.gpio_irq));
	return 0;
}

int gxfp_51b7_write(struct gxfp_dev *g, const u8 *packet, size_t len)
{
	acpi_handle ec;
	unsigned long deadline;
	u8 cmd;
	int ret;

	if (g->transport_fault)
		return -EIO;
	if (!packet || len < 4 || len > 512 ||
	    get_unaligned_le16(packet + 1) != len - 4 ||
	    (u8)(packet[0] + packet[1] + packet[2]) != packet[3] ||
	    (packet[0] != 0xa0 && packet[0] != 0xb0))
		return -EINVAL;
	if (ACPI_FAILURE(acpi_get_handle(NULL, EC_PATH, &ec)))
		return -ENODEV;
	/*
	 * Hold the AML mutex across wait + write so AML's own CMDR accesses
	 * (ECRD/ECWT) cannot interleave with ours. AML ECMD takes ECMT per
	 * access, not across its own wait + write, so it is not atomic itself;
	 * on this firmware only the sensor's _DSM functions 2..4 call it,
	 * and nothing on Linux does. ECMD itself is avoided: its wait is
	 * unbounded.
	 */
	if (ACPI_FAILURE(acpi_acquire_mutex(ec, "ECMT", 100)))
		return -ETIMEDOUT;
	deadline = jiffies + msecs_to_jiffies(100);
	for (;;) {
		ret = ec_read(CMDR, &cmd);
		if (ret || !cmd)
			break;
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		usleep_range(100, 200);
	}
	if (!ret) {
		/* EC copies the entire 512-byte TX slot; initialise its padding. */
		memset_io(g->hw.mailbox_mmio, 0, 512);
		memcpy_toio(g->hw.mailbox_mmio, packet, len);
		/* The frame must be in the mailbox before the EC is told to read it. */
		wmb();
		ret = ec_write(CMDR, 0xf0);
	}
	acpi_release_mutex(ec, "ECMT");
	return ret;
}

irqreturn_t gxfp_51b7_irq_thread(int irq, void *data)
{
	struct gxfp_dev *g = data;
	unsigned long deadline;
	u16 body_len;
	size_t len;
	int raw, ret = 0;

	mutex_lock(&g->lock);
	raw = gpiod_get_raw_value_cansleep(g->hw.gpio_irq);
	if (raw > 0)
		goto out;
	if (raw < 0) {
		ret = raw;
		goto fault;
	}
	memcpy_fromio(g->irq.rx_buf, g->hw.mailbox_mmio + RX_OFF, 4);
	if (!gxfp_check_mp_head(g->irq.rx_buf, 4, &body_len)) {
		ret = -EBADMSG;
		goto fault;
	}
	len = 4u + body_len;
	if (!body_len || len > g->hw.mailbox_size - RX_OFF || len > g->irq.rx_cap) {
		ret = -EMSGSIZE;
		goto fault;
	}
	memcpy_fromio(g->irq.rx_buf + 4, g->hw.mailbox_mmio + RX_OFF + 4, body_len);
	/* Finish reading the slot before the ACK lets the EC reuse it. */
	mb();
	gpiod_set_raw_value_cansleep(g->hw.gpio_read_done, 0);
	/* Firmware raises IRQ when it has observed the low ACK. */
	deadline = jiffies + msecs_to_jiffies(20);
	do {
		raw = gpiod_get_raw_value_cansleep(g->hw.gpio_irq);
		if (raw != 0)
			break;
		usleep_range(100, 200);
	} while (time_before(jiffies, deadline));
	usleep_range(5000, 5500);
	gpiod_set_raw_value_cansleep(g->hw.gpio_read_done, 1);
	if (raw <= 0) {
		ret = raw < 0 ? raw : -ETIMEDOUT;
		goto fault;
	}
	gxfp_uapi_rxq_push_packet(g, g->irq.rx_buf, len);
	goto out;
fault:
	/* Fail closed instead of repeatedly handling a stuck level IRQ. */
	g->transport_fault = true;
	disable_irq_nosync(irq);
	dev_err(g->dev, "51B7 receive fault %d; IRQ stopped (reload required)\n", ret);
out:
	mutex_unlock(&g->lock);
	return IRQ_HANDLED;
}
