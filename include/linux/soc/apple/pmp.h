/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_H
#define _LINUX_SOC_APPLE_PMP_H
#include <linux/types.h>

int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled);
int apple_pmp_lab_register(const void *data);
void apple_pmp_lab_unregister(const void *data);
int apple_pmp_lab_send_command(const void *data, u8 command, u16 device_id,
			       u32 enabled);
#endif
