// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/soc/apple/pmp.h>

/* Serialize the untagged power reply stream and join callers on removal. */
static DEFINE_MUTEX(pmp_lock);
static const void *pmp_data;
static struct device *pmp_device;
static unsigned long pmp_registered_at;

/*
 * The PMP firmware still sets itself up for about 100 ms after its endpoint
 * comes up; a power request in that window crashes it (J613: a GPU vote
 * 0.03 ms after the endpoint, PC=0). Wait for the setup and the PTD ready bit.
 */
#define PMP_SETUP_MS		250
#define PMP_READY_TIMEOUT_MS	5000
int apple_pmp_report_wait_ready(unsigned int timeout_ms);

int apple_pmp_register(struct device *dev, const void *data)
{
	int ret = 0;
	mutex_lock(&pmp_lock);
	if (pmp_data)
		ret = -EBUSY;
	else {
		pmp_data = data;
		pmp_device = dev;
		pmp_registered_at = jiffies;
	}
	mutex_unlock(&pmp_lock);
	return ret;
}

void apple_pmp_unregister(const void *data)
{
	mutex_lock(&pmp_lock);
	if (pmp_data == data) {
		pmp_data = NULL;
		pmp_device = NULL;
	}
	mutex_unlock(&pmp_lock);
}

int apple_pmp_link_device(struct device *consumer)
{
	struct device_node *node;
	struct device *supplier = NULL;
	struct device_link *link;

	node = of_parse_phandle(consumer->of_node, "apple,pmp", 0);
	if (!node)
		return -EINVAL;
	mutex_lock(&pmp_lock);
	if (pmp_device && pmp_device->of_node == node && device_is_bound(pmp_device))
		supplier = get_device(pmp_device);
	mutex_unlock(&pmp_lock);
	of_node_put(node);
	if (!supplier)
		return -EPROBE_DEFER;

	/* Firmware must remain available through the consumer's remove callback. */
	link = device_link_add(consumer, supplier, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(supplier);
	return link ? 0 : -EINVAL;
}
EXPORT_SYMBOL_GPL(apple_pmp_link_device);

int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled)
{
	int ret;
	if ((command != 0x0e && command != 0x0f) || device_id > 15 || enabled > 1)
		return -EINVAL;
	mutex_lock(&pmp_lock);
	if (pmp_data) {
		unsigned long ready = pmp_registered_at + msecs_to_jiffies(PMP_SETUP_MS);
		struct device *dev = pmp_device;

		mutex_unlock(&pmp_lock);
		if (time_before(jiffies, ready)) {
			dev_info(dev, "waiting %u ms for PMP setup before power request\n",
				 jiffies_to_msecs(ready - jiffies));
			msleep(jiffies_to_msecs(ready - jiffies) + 1);
		}
		ret = apple_pmp_report_wait_ready(PMP_READY_TIMEOUT_MS);
		/* The status word is unverified on every SoC: only defer is fatal. */
		if (ret == -EPROBE_DEFER)
			return ret;
		if (ret)
			dev_warn(dev, "PMP ready bit not seen (%d); sending power request anyway\n", ret);
		mutex_lock(&pmp_lock);
	}
	ret = pmp_data ? apple_pmp_send_power_command(pmp_data, command,
						     device_id, enabled) : -ENODEV;
	mutex_unlock(&pmp_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmp_set_device_power);
