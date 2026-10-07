// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Explicit runtime gate for the guarded, otherwise disabled J613/25G83 DT.
 * No firmware messages, reset, compatible aliases, or teardown. Reboot to
 * discard the activation and retained DART/RTKit resources.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>

static unsigned int display_clock_hz;
module_param(display_clock_hz, uint, 0400);
MODULE_PARM_DESC(display_clock_hz, "Required same-boot proxy ADT observation");
static struct of_changeset gate;

static int __init activate(void)
{
	const char *paths[] = { "/soc/iommu@28d30c000", "/soc/iommu@28d304000",
		"/soc/mailbox@28ec08000", "/soc/display-subsystem",
		"/soc/dcp@28ec00000/piodma", "/soc/dcp@28ec00000" };
	const char *domains[] = { "1d8", "208", "10000" };
	struct device_node *nodes[6] = {}, *np;
	const char *s;
	void __iomem *cpu;
	char path[128];
	u32 marker, version[3];
	int i, ret = -ENODEV;

	if (!display_clock_hz || !of_machine_is_compatible("apple,j613"))
		return ret;
	for (i = 0; i < ARRAY_SIZE(domains); i++) {
		snprintf(path, sizeof(path), "/soc/power-management@2d0700000/power-controller@%s", domains[i]);
		np = of_find_node_by_path(path);
		ret = np && of_property_read_bool(np, "apple,inherited-on") ? 0 : -EPERM;
		of_node_put(np);
		if (ret)
			goto out;
	}
	for (i = 0; i < ARRAY_SIZE(nodes); i++) {
		nodes[i] = of_find_node_by_path(paths[i]);
		if (!nodes[i] || of_property_present(nodes[i], "apple,j613-25g83-display-handoff") || of_property_read_string(nodes[i], "status", &s) || strcmp(s, "disabled")) {
			ret = -EBUSY;
			goto out;
		}
	}
	if (!of_device_is_compatible(nodes[5], "apple,t8122-dcp") ||
	    of_property_read_string(nodes[5], "apple,firmware-uuid", &s) ||
	    strcmp(s, "C042E95C-B9D8-3F0E-94B3-582A08AA6FDD") ||
	    of_property_read_u32_array(nodes[5], "apple,firmware-compat", version, 3) ||
	    version[0] != 26 || version[1] != 6 || version[2] != 2 ||
	    of_property_read_u32(nodes[5], "apple,j613-25g83-mapping-handoff", &marker) || marker != 1) {
		ret = -EPERM;
		goto out;
	}
	cpu = of_iomap(nodes[5], 0);
	if (!cpu) {
		ret = -ENOMEM;
		goto out;
	}
	ret = readl(cpu + 0x44) & BIT(4) ? 0 : -EBUSY;
	iounmap(cpu);
	if (ret)
		goto out;
	of_changeset_init(&gate);
	for (i = 3; i < ARRAY_SIZE(nodes) && !ret; i++)
		ret = of_changeset_add_prop_u32(&gate, nodes[i], "apple,j613-25g83-display-handoff", 1);
	for (i = 0; i < ARRAY_SIZE(nodes) && !ret; i++)
		ret = of_changeset_update_prop_string(&gate, nodes[i], "status", "okay");
	if (!ret)
		ret = of_changeset_apply(&gate);
	if (ret)
		of_changeset_destroy(&gate);
	else {
		__module_get(THIS_MODULE);
		pr_info("J613/25G83 guarded appledrm activation; ADT clock %u; retained until reboot\n", display_clock_hz);
	}
out:
	for (i = 0; i < ARRAY_SIZE(nodes); i++)
		of_node_put(nodes[i]);
	return ret;
}
module_init(activate);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Explicit guarded J613/25G83 appledrm runtime activation");
