// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Bounded J713 RTKit diagnostic. No DART, CPU-control or display-register writes. */
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define DCP_BASE 0x30ae00000ULL
#define MBOX_BASE 0x30ae08000ULL
#define WINDOW_SIZE 0x4000
#define MSG_TYPE GENMASK_ULL(59, 52)

static bool power_on;
module_param(power_on, bool, 0400);
MODULE_PARM_DESC(power_on, "Send one IOP power-on request to an already running DCP");

static int __init m4_dcp_probe_init(void)
{
	struct device_node *dcp, *mbox;
	struct resource res;
	void __iomem *cpu = NULL, *fifo = NULL;
	const char *uuid;
	u64 message;
	u32 control, status, tx, rx, endpoint;
	int ret = -ENODEV;

	if (!of_machine_is_compatible("apple,j713") ||
	    !of_machine_is_compatible("apple,t8132"))
		return -ENODEV;
	dcp = of_find_node_by_path("/soc/dcp@30ae00000");
	mbox = of_find_node_by_path("/soc/mailbox@30ae08000");
	if (!dcp || !mbox || of_device_is_available(dcp) ||
	    of_device_is_available(mbox) ||
	    !of_device_is_compatible(dcp, "apple,t8132-dcp") ||
	    !of_device_is_compatible(mbox, "apple,asc-mailbox-v4") ||
	    of_property_read_string(dcp, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, "81BAF476-679D-3CC0-8C6A-E6B6FA659D5E"))
		goto out_nodes;
	if (of_address_to_resource(dcp, 0, &res) ||
	    res.start != DCP_BASE || resource_size(&res) != WINDOW_SIZE ||
	    of_address_to_resource(mbox, 0, &res) ||
	    res.start != MBOX_BASE || resource_size(&res) != WINDOW_SIZE)
		goto out_nodes;
	ret = -EBUSY;
	if (!request_mem_region(MBOX_BASE, WINDOW_SIZE, "m4-dcp-probe"))
		goto out_nodes;
	cpu = ioremap(DCP_BASE, WINDOW_SIZE);
	fifo = ioremap(MBOX_BASE, WINDOW_SIZE);
	ret = -ENOMEM;
	if (!cpu || !fifo)
		goto out_map;
	control = readl(cpu + 0x44);
	status = readl(cpu + 0x48);
	tx = readl(fifo + 0x110);
	rx = readl(fifo + 0x114);
	pr_info("m4_dcp_probe: control=%#x status=%#x tx=%#x rx=%#x power_on=%d\n",
		control, status, tx, rx, power_on);
	ret = 0;
	if (!power_on)
		goto out_map;
	/* Pending traffic belongs to the inherited session; do not consume it. */
	ret = -EBUSY;
	if (!(control & BIT(4)) || tx & BIT(16) || !(rx & BIT(17)))
		goto out_map;
	message = FIELD_PREP(MSG_TYPE, 6) | 0x20;
	wmb();
	writeq(message, fifo + 0x800);
	writeq(0, fifo + 0x808);
	pr_info("m4_dcp_probe: tx endpoint=0 message=%#llx\n", message);
	ret = readl_poll_timeout(fifo + 0x114, rx, !(rx & BIT(17)), 100, 2000000);
	if (ret)
		goto out_map;
	message = readq(fifo + 0x830);
	endpoint = (u32)readq(fifo + 0x838);
	rmb();
	pr_info("m4_dcp_probe: rx endpoint=%u message=%#llx\n", endpoint, message);
	ret = endpoint || FIELD_GET(MSG_TYPE, message) != 7 ||
	      (message & 0xffff) != 0x20 ? -EPROTO : 0;
	if (!ret)
		pr_info("m4_dcp_probe: inherited RTKit acknowledged IOP power ON\n");
out_map:
	if (fifo)
		iounmap(fifo);
	if (cpu)
		iounmap(cpu);
	release_mem_region(MBOX_BASE, WINDOW_SIZE);
out_nodes:
	of_node_put(mbox);
	of_node_put(dcp);
	pr_info("m4_dcp_probe: completed ret=%d\n", ret);
	return ret;
}

static void __exit m4_dcp_probe_exit(void) { }
module_init(m4_dcp_probe_init);
module_exit(m4_dcp_probe_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Bounded J713 DCP RTKit power acknowledgement diagnostic");
