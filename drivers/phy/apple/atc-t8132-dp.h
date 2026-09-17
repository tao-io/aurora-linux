/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_ATCPHY_T8132_DP_H
#define APPLE_ATCPHY_T8132_DP_H

#include <linux/usb/typec.h>

struct apple_t8132_dp;
struct mutex;
struct phy;
struct platform_device;

/* NULL means that the DT supplies only the existing USB2 configuration.
 * power_on/off owns the common PHY session; set_mode(DP/INVALID, 0) toggles
 * AUX without resetting the common PHY. Stop the main link and crossbar first.
 * Until DCP shutdown is qualified, retain the power reference through its
 * quiescence; an AUX deactivation is not a release of that obligation.
 */
struct apple_t8132_dp *apple_t8132_dp_create(struct platform_device *pdev,
					  struct mutex *lock, struct phy **phy);
/* Called with the shared ATC lock held. These never wait for DCP or change
 * active lanes. The display consumer receives the same Type-C notification
 * and owns link/crossbar retirement before phy_power_off().
 */
void apple_t8132_dp_orientation(struct apple_t8132_dp *dp,
			       enum typec_orientation orientation);
int apple_t8132_dp_route(struct apple_t8132_dp *dp, bool pin_d);
int apple_t8132_dp_suspend(struct apple_t8132_dp *dp);

#endif
