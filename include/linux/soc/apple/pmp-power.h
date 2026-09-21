/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_POWER_H
#define _LINUX_SOC_APPLE_PMP_POWER_H

/* J713 power-policy -> G16 ceiling mailbox. No dependency on thermal module
 * lifetime: the PMP report provider owns the mailbox and stale protection.
 */
void apple_pmp_power_publish_gpu(unsigned int level);
unsigned int apple_pmp_power_gpu_state(void);

#endif
