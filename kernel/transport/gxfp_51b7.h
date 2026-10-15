/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GXFP_51B7_H
#define GXFP_51B7_H

#include <linux/interrupt.h>
#include <linux/platform_device.h>

struct gxfp_dev;

int gxfp_51b7_init_gpios(struct platform_device *pdev);
int gxfp_51b7_write(struct gxfp_dev *gdev, const u8 *packet, size_t len);
irqreturn_t gxfp_51b7_irq_thread(int irq, void *data);

#endif /* GXFP_51B7_H */
