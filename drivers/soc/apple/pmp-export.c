// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/soc/apple/pmp.h>

/* Serialize lab access against driver removal; the Rust driver owns data. */
static DEFINE_MUTEX(pmp_lab_lock);
static const void *pmp_lab_data;

int apple_pmp_lab_register(const void *data)
{
	int ret = 0;
	mutex_lock(&pmp_lab_lock);
	if (pmp_lab_data)
		ret = -EBUSY;
	else
		pmp_lab_data = data;
	mutex_unlock(&pmp_lab_lock);
	return ret;
}

void apple_pmp_lab_unregister(const void *data)
{
	mutex_lock(&pmp_lab_lock);
	if (pmp_lab_data == data)
		pmp_lab_data = NULL;
	mutex_unlock(&pmp_lab_lock);
}

int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled)
{
	int ret;
	if ((command != 0x0e && command != 0x0f) || device_id > 15 || enabled > 1)
		return -EINVAL;
	mutex_lock(&pmp_lab_lock);
	ret = pmp_lab_data ? apple_pmp_lab_send_command(pmp_lab_data, command,
						     device_id, enabled) : -ENODEV;
	mutex_unlock(&pmp_lab_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmp_set_device_power);
