// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Isolate an AGX PMP vote from GPU firmware startup; explicit guarded test. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/apple/pmp.h>
#include <linux/unaligned.h>

static int __init pmp_probe(void)
{
	struct device_node *node;
	const u8 *table;
	void __iomem *cpu;
	u32 before, after;
	int length, ret = -EINVAL;

	if (!of_machine_is_compatible("apple,j713"))
		return -ENODEV;
	node = of_find_node_by_path("/soc/pmp@380500000");
	if (!node)
		return -ENODEV;
	table = of_get_property(node, "apple,tunable-soc-device", &length);
	if (table && length == 34 * 0x7c && get_unaligned_le32(table + 12 * 0x7c) == 5 &&
	    !memcmp(table + 12 * 0x7c + 0x74, "AGX\0\0\0\0\0", 8))
		ret = 0;
	of_node_put(node);
	if (ret)
		return ret;
	cpu = ioremap(0x30ae00000ULL, 0x4000);
	if (!cpu)
		return -ENOMEM;
	before = readl(cpu + 0x44);
	ret = (before & BIT(4)) ? apple_pmp_set_device_power(0x0f, 5, 1) : -EBUSY;
	if (!ret)
		msleep(100);
	after = readl(cpu + 0x44);
	pr_info("M4 isolated AGX vote ret=%d DCP before=%#x after=%#x running=%d\n",
		ret, before, after, !!(after & BIT(4)));
	iounmap(cpu);
	return ret ?: (after & BIT(4) ? 0 : -EIO);
}

static void __exit pmp_probe_exit(void) { }
module_init(pmp_probe);
module_exit(pmp_probe_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J713 isolated AGX power vote and DCP liveness test");
