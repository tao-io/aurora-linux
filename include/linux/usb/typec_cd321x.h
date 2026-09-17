/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_USB_TYPEC_CD321X_H
#define __LINUX_USB_TYPEC_CD321X_H
#include <linux/types.h>
struct device;
struct cd321x_dp_snapshot {
	u32 status;
	u32 data_status;
	u32 dp_status;
	/* Counts DATA_STATUS_UPDATE interrupts carrying HPD IRQ, not cache reads. */
	u64 hpd_irqs;
};
/* Process context; takes device/controller locks. Refreshes DP SID at most once
 * per second while DP is negotiated; IRQ events are counted separately. */
int cd321x_get_dp_state(struct device *dev, struct cd321x_dp_snapshot *snapshot);
#endif
