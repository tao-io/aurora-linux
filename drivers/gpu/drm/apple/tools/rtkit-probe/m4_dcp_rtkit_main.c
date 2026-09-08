// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Explicit-load J713 DCP bring-up client using the native DMA and RTKit APIs. */
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/rtkit.h>

#include "../../dcp-link.h"
#include "m4_dcp_bridge.h"

struct m4_dcp_rtkit {
	struct device *dev;
	struct apple_rtkit *rtkit;
	struct completion ready;
	void *rpc;
	dma_addr_t rpc_dva;
	int result;
	bool awaiting_rpc;
	struct m4_dcp_bridge *bridge;
};

static bool start_link;
module_param(start_link, bool, 0400);
MODULE_PARM_DESC(start_link, "Also negotiate DCPLink, without issuing display RPCs");
static bool query_display;
module_param(query_display, bool, 0400);
MODULE_PARM_DESC(query_display, "Issue the M4 A412 read-only main-display query after READY");
static bool bridge;
module_param(bridge, bool, 0400);
MODULE_PARM_DESC(bridge, "Expose a root-only diagnostic RPC bridge after DCPLink READY");

static void crashed(void *cookie, const void *log, size_t size)
{
	struct m4_dcp_rtkit *dcp = cookie;

	dcp->result = -EIO;
	if (dcp->bridge)
		m4_dcp_bridge_crashed(dcp->bridge);
	dev_err(dcp->dev, "DCP firmware crashed; retain all DMA allocations until reboot\n");
	complete(&dcp->ready);
}

static void receive(void *cookie, u8 ep, u64 message)
{
	struct m4_dcp_rtkit *dcp = cookie;
	u16 alignment;
	u32 hash;

	dev_info(dcp->dev, "DCP app rx ep=%#x message=%#llx\n", ep, message);
	if (ep != APPLE_DCP_LINK_ENDPOINT)
		return;
	if (dcp->bridge) {
		m4_dcp_bridge_receive(dcp->bridge, message);
		return;
	}
	if (dcp->awaiting_rpc) {
		u32 output;

		dma_rmb();
		output = le32_to_cpup((__le32 *)(dcp->rpc + 12));
		if (message != APPLE_DCP_LINK_MSG_RPC_REPLY || output > 1) {
			dcp->result = -EPROTO;
		} else {
			dcp->result = 0;
			dev_info(dcp->dev, "M4 A412 reply: is_main_display=%u\n", output);
		}
		dcp->awaiting_rpc = false;
		complete(&dcp->ready);
		return;
	}
	if ((message & 3) == APPLE_DCP_LINK_MSG_FIRMWARE_INIT)
		return;
	/* On this M4 firmware the upper field is the shared-memory alignment.
	 * __TEXT 0x19843c reads descriptor+12 and takes max(host, cacheline=64).
	 * It is not a protocol version; do not clamp the reply to version 4.
	 */
	if (apple_dcp_link_ready_reply(message, &alignment, &hash) ||
	    alignment != 64 || hash != 0x45bc9ef4) {
		dcp->result = -EPROTO;
	} else {
		dcp->result = 0;
		dev_info(dcp->dev, "M4 DCPLink READY alignment=%u hash=%#x\n", alignment, hash);
	}
	complete(&dcp->ready);
}

static int shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct m4_dcp_rtkit *dcp = cookie;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);
	phys_addr_t phys;

	if (!bfr->size || bfr->size > SZ_16M || !domain)
		return -EINVAL;
	if (!bfr->iova) {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size, &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;
		memset(bfr->buffer, 0, bfr->size);
		dma_wmb();
		dev_info(dcp->dev, "DCP shmem allocated size=%#zx dva=%pad\n",
			 bfr->size, &bfr->iova);
		return 0;
	}
	phys = iommu_iova_to_phys(domain, bfr->iova);
	/* Restrict firmware-supplied addresses to a declared reserved region. */
	for (int i = 0; ; i++) {
		struct device_node *node = of_parse_phandle(dcp->dev->of_node, "memory-region", i);
		struct resource res;
		bool oslog, valid;

		if (!node)
			return -ERANGE;
		oslog = of_property_read_bool(node, "apple,dcp-os-log");
		valid = !of_address_to_resource(node, 0, &res);
		of_node_put(node);
		if (!valid)
			continue;
		/* The OS-log carveout is a host physical buffer, not a DART alias. */
		if (oslog && bfr->iova == res.start)
			phys = res.start;
		if (phys < res.start || phys > res.end || bfr->size - 1 > res.end - phys)
			continue;
		if (!oslog) {
			for (size_t off = 0; off < bfr->size; off += SZ_16K)
				if (iommu_iova_to_phys(domain, bfr->iova + off) != phys + off)
					return -ERANGE;
			if (iommu_iova_to_phys(domain, bfr->iova + bfr->size - 1) != phys + bfr->size - 1)
				return -ERANGE;
		}
		bfr->buffer = memremap(phys, bfr->size, MEMREMAP_WB);
		if (!bfr->buffer)
			return -ENOMEM;
		bfr->is_mapped = true;
		dev_info(dcp->dev, "DCP shmem inherited size=%#zx dva=%pad phys=%pap\n",
			 bfr->size, &bfr->iova, &phys);
		return 0;
	}
}

static void shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct m4_dcp_rtkit *dcp = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops ops = {
	.crashed = crashed,
	.recv_message = receive,
	.shmem_setup = shmem_setup,
	.shmem_destroy = shmem_destroy,
};

static int probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct m4_dcp_rtkit *dcp;
	void __iomem *cpu;
	const char *uuid;
	u64 message;
	int ret;

	if (!of_machine_is_compatible("apple,j713") ||
	    of_property_read_string(dev->of_node, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, "81BAF476-679D-3CC0-8C6A-E6B6FA659D5E"))
		return -ENODEV;
	if (!iommu_get_domain_for_dev(dev))
		return -ENODEV;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;
	cpu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cpu))
		return PTR_ERR(cpu);
	if (!(readl(cpu + 0x44) & BIT(4)))
		return dev_err_probe(dev, -EBUSY, "DCP CPU is not already running\n");
	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;
	dcp->dev = dev;
	init_completion(&dcp->ready);
	platform_set_drvdata(pdev, dcp);
	dcp->rtkit = apple_rtkit_init(dev, dcp, "mbox", 0, &ops);
	if (IS_ERR(dcp->rtkit))
		return dev_err_probe(dev, PTR_ERR(dcp->rtkit), "RTKit init failed\n");
	/* This bring-up client has no qualified hot teardown. Retain its DMA and
	 * RTKit objects even on startup failure; recovery is a guarded reboot.
	 */
	__module_get(THIS_MODULE);
	ret = apple_rtkit_wake(dcp->rtkit);
	if (ret)
		goto retained;
	dev_info(dev, "M4 native RTKit boot complete; running=%d\n", apple_rtkit_is_running(dcp->rtkit));
	if (!start_link && !query_display && !bridge)
		goto retained;
	dcp->rpc = dma_alloc_coherent(dev, APPLE_DCP_LINK_RPC_MEMORY_SIZE,
				      &dcp->rpc_dva, GFP_KERNEL);
	if (!dcp->rpc) {
		ret = -ENOMEM;
		goto retained;
	}
	memset(dcp->rpc, 0, APPLE_DCP_LINK_RPC_MEMORY_SIZE);
	apple_dcp_link_init_desc_encode(dcp->rpc, 0, false);
	/* The imported descriptor names this field version; M4 uses alignment. */
	((struct apple_dcp_link_init_desc *)dcp->rpc)->version = cpu_to_le32(64);
	ret = apple_dcp_link_init_message(dcp->rpc_dva, &message);
	if (ret)
		goto retained;
	dcp->result = -ETIMEDOUT;
	ret = apple_rtkit_start_ep(dcp->rtkit, APPLE_DCP_LINK_ENDPOINT);
	if (ret)
		goto retained;
	dma_wmb();
	ret = apple_rtkit_send_message(dcp->rtkit, APPLE_DCP_LINK_ENDPOINT, message, NULL, false);
	if (ret)
		goto retained;
	if (!wait_for_completion_timeout(&dcp->ready, msecs_to_jiffies(3000)))
		ret = -ETIMEDOUT;
	else
		ret = dcp->result;
	if (!ret && query_display) {
		/* __TEXT 0x1e2480: no input, output >= 4; reads one display flag.
		 * One request on AP stream 0 at offset 0, already 64-byte aligned.
		 */
		reinit_completion(&dcp->ready);
		apple_dcp_link_rpc_header_encode(dcp->rpc, 0x41343132, 0, 4);
		*(__le32 *)(dcp->rpc + 12) = cpu_to_le32(0xa5);
		dcp->result = -ETIMEDOUT;
		dcp->awaiting_rpc = true;
		dma_wmb();
		ret = apple_rtkit_send_message(dcp->rtkit, APPLE_DCP_LINK_ENDPOINT,
					      (16ULL << 32) | APPLE_DCP_LINK_MSG_RPC, NULL, false);
		if (!ret) {
			if (!wait_for_completion_timeout(&dcp->ready, msecs_to_jiffies(3000)))
				ret = -ETIMEDOUT;
			else
				ret = dcp->result;
		}
	}
	if (!ret && bridge) {
		struct m4_dcp_bridge *rpc_bridge = m4_dcp_bridge_create(dev, dcp->rtkit, dcp->rpc, false);

		if (IS_ERR(rpc_bridge))
			ret = PTR_ERR(rpc_bridge);
		else {
			dcp->bridge = rpc_bridge;
			m4_dcp_bridge_activate(rpc_bridge);
		}
	}
retained:
	dcp->result = ret;
	dev_info(dev, "M4 native DCP diagnostic finished ret=%d; resources retained until reboot\n", ret);
	return 0;
}

static const struct of_device_id matches[] = {
	{ .compatible = "apple,t8132-dcp" },
	{ }
};
/* Deliberately no module alias: this diagnostic is loaded explicitly. */
static struct platform_driver m4_dcp_driver = {
	.probe = probe,
	.driver = {
		.name = "m4-dcp-rtkit",
		.of_match_table = matches,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(m4_dcp_driver);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J713 native RTKit/DMA DCPLink startup diagnostic");
