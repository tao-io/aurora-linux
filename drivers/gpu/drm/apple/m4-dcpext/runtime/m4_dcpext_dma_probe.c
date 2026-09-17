// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J713 external DART additive mapping diagnostic. No firmware messages.
 * PTE encoding and ordering: Asahi io-pgtable-dart.c, fba91e5d9577.
 * T8110 SID flush: Asahi apple-dart.c, same revision.
 * Reservations: our m1n1 ab71c64 handoff, derived from J713 ADT.
 * This is not a DMA execution test: the private DVA is never sent to DCP.
 */
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/kthread.h>

#define LEASE_PAGE 0x4000
#ifdef DCPEXT_WITH_SESSION
#ifdef DCPEXT_DESKTOP
static const bool desktop_requested = true;
static bool desktop;
static unsigned int desktop_test_ms;
module_param(desktop_test_ms, uint, 0400);
MODULE_PARM_DESC(desktop_test_ms, "0 normal desktop; 20000 or 30000 bounded qualification");
static bool desktop_stop;
module_param(desktop_stop, bool, 0600);
MODULE_PARM_DESC(desktop_stop, "Request acknowledged display shutdown; restart remains TODO");
#else
static const bool desktop_requested;
static bool desktop;
static const unsigned int desktop_test_ms;
static const bool desktop_stop;
#endif
static bool native_kms_trial;
module_param(native_kms_trial, bool, 0400);
static bool native_query;
static bool native_startup;
static bool native_close_before_route;
module_param(native_close_before_route, bool, 0400);
static bool native_mode_trial;
module_param(native_mode_trial, bool, 0400);
static bool native_power_trial;
module_param(native_power_trial, bool, 0400);
static bool native_background_trial;
module_param(native_background_trial, bool, 0400);
static bool native_frame_trial;
module_param(native_frame_trial, bool, 0400);
static bool native_route;
module_param(native_route, bool, 0400);
static bool native_sleep_cycle;
module_param(native_sleep_cycle, bool, 0400);
module_param(native_startup, bool, 0400);
module_param(native_query, bool, 0400);
MODULE_PARM_DESC(native_query, "Bounded DCPLink READY/A412 external profile query; excludes all AFK/PHY/image options");
#define LEASE_SIZE (native_startup ? 0x200000 : native_query ? 0x100000 : 0x40000)
#else
#define LEASE_SIZE 0x10000
#endif
#define LEASE_DVA  0x40000000ULL
#define LEASE_SLOT (LEASE_DVA >> 25)
#define PTE_ADDRESS GENMASK_ULL(39, 10)
#define PTE_RW (GENMASK_ULL(51, 40) | 1)

static unsigned int port;
module_param(port, uint, 0400);
static int result = -EINPROGRESS;
module_param(result, int, 0400);
MODULE_PARM_DESC(result, "0 only after private mapping removal and inherited root comparison");
static struct device *dma_dev;
static void *pool, *leaf;
static dma_addr_t pool_dma;
static u64 *root, *saved;
static void __iomem *regs;
static phys_addr_t dart_base;
static bool region_owned, pinned;

static u64 encode_address(phys_addr_t address)
{
	return (address >> 4) & PTE_ADDRESS;
}

static int fabric_powered(void);
static int remove_private_mapping(void);
static void cleanup(void);

#include "dcpext_fe_map.h"
#include "dcpext_frame_map.h"
#include "dcpext_pio_map.h"
#ifdef DCPEXT_WITH_SESSION
#include "dcpext_session_adapter.h"
#endif

static int flush_sid(void)
{
	u32 status;

	/* DART table walks are coherent, as in apple_dart_finalize_domain. */
	dma_wmb();
	writel(0x100 | 23, regs + 0x80);
	return readl_poll_timeout(regs + 0x80, status, !(status & BIT(31)), 1, 10000);
}

static int fabric_powered(void)
{
	struct device_node *node;
	struct regmap *map;
	u32 state, i, offset = port ? 0x4e0 : 0x1d8;
	int ret;

	node = of_find_node_by_path("/soc/power-management@380700000");
	if (!node || !of_device_is_compatible(node, "apple,t8132-pmgr")) {
		of_node_put(node);
		return -ENODEV;
	}
	map = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(map))
		return PTR_ERR(map);
	for (i = 0; i < 2; i++) {
		ret = regmap_read(map, offset + i * 8, &state);
		if (ret)
			return ret;
		if ((state & 0xff) != 0xff ||
		    state & (BIT(31) | BIT(28) | BIT(12) | BIT(11) | BIT(10)))
			return -EHOSTDOWN;
	}
	return 0;
}

static void cleanup(void)
{
#ifdef DCPEXT_WITH_SESSION
	session_cleanup();
#endif
	pio_cleanup();
	frame_cleanup();
	fe_cleanup();
	if (root)
		memunmap(root);
	if (leaf)
		free_page((unsigned long)leaf);
	if (pool)
		dma_free_coherent(dma_dev, LEASE_SIZE, pool, pool_dma);
	if (dma_dev)
		root_device_unregister(dma_dev);
	if (regs)
		iounmap(regs);
	if (region_owned)
		release_mem_region(dart_base, 0x20000);
	kfree(saved);
}

/* Called only after protocol completion, or by the no-firmware mapper. On
 * any mismatch all allocations and the module reference remain owned. */
static int remove_private_mapping(void)
{
	u64 entry = encode_address(virt_to_phys(leaf)) | 1;
	unsigned int i;
	int ret;

	/* Verify only our private entries; inherited mappings must be unchanged. */
	for (i = 0; i < LEASE_SIZE / LEASE_PAGE; i++)
		if ((((u64 *)leaf)[i] & PTE_ADDRESS) << 4 != pool_dma + i * LEASE_PAGE) {
			ret = -EIO;
			return ret;
		}
	ret = pio_remove();
	if (ret)
		return ret;
	ret = frame_remove();
	if (ret)
		return ret;
	ret = -EAGAIN;
	if (cmpxchg64_relaxed(&root[LEASE_SLOT], entry, 0) != entry)
		return ret;
	ret = flush_sid();
	if (ret)
		return ret;
	if (memcmp(root, saved, LEASE_PAGE) || readl(regs + 0x100)) {
		ret = -EIO;
		return ret;
	}
#ifdef DCPEXT_WITH_SESSION
	if (memcmp(session_table_map, session_table_saved, resource_size(&session_table_ram))) {
		ret = -EIO;
		return ret;
	}
#endif
	return 0;
}

/* Built-in desktop startup runs after init memory has been freed. */
static int probe_init(void)
{
	struct device_node *dcp = NULL, *dart = NULL, *ram = NULL;
	struct resource resource, reservation;
	phys_addr_t root_pa, leaf_pa;
	const char *uuid;
	char alias[16];
	u32 marker, ttbr, tcr, protect, params[2];
	u64 entry;
	int count, segment_bytes, i, ret = -ENODEV;

#ifdef DCPEXT_WITH_SESSION
#ifdef DCPEXT_DESKTOP
	/* First usable desktop: exactly the already-qualified connected route.
	 * Keep ownership pinned until explicit stop or shutdown; no sleep tests.
	 */
	if (port || (desktop_test_ms && desktop_test_ms != 20000 && desktop_test_ms != 30000) ||
	    sleep_iop || restart_cpu || native_sleep_cycle || audio_probe || iboot_probe ||
	    crash_on_quiesce_stall || capture_power_state || native_stop_signal ||
	    native_close_before_route || native_background_trial || quiesce_with_route ||
	    quiesce_with_power || observe_slow_quiesce || release_before_hpd_off ||
	    quiesce_before_afk_stop)
		return -EINVAL;
	native_kms_trial = native_query = native_startup = native_route = true;
	native_mode_trial = native_power_trial = native_frame_trial = true;
	native_system_shutdown = edid_probe = pio_map = frame_map = true;
	link_training = link_capabilities = true;
	afk_endpoint = 0x2a;
	afk_query = 7;
	frame_buffers = 2;
#endif
	if (native_startup && (!native_query || !pio_map))
		return -EINVAL;
	if (native_close_before_route && !native_route)
		return -EINVAL;
	if (native_mode_trial && (!native_route || native_close_before_route))
		return -EINVAL;
	if (native_power_trial && !native_mode_trial)
		return -EINVAL;
	if (capture_power_state && (!native_startup || native_kms_trial))
		return -EINVAL;
	if (crash_on_quiesce_stall && (!native_route || !native_startup || native_kms_trial))
		return -EINVAL;
	if (native_background_trial && !native_power_trial)
		return -EINVAL;
	if (native_kms_trial && (!native_frame_trial || !edid_probe || frame_buffers != 2))
		return -EINVAL;
	if (native_frame_trial && (!native_power_trial || !frame_map || native_background_trial))
		return -EINVAL;
	if (native_route && (!native_startup || afk_endpoint != 0x2a ||
	    afk_query != 7 || !link_training || !link_capabilities))
		return -EINVAL;
	if (native_sleep_cycle && (!native_route || !native_startup ||
	    !native_system_shutdown || !sleep_iop || !restart_cpu || native_kms_trial))
		return -EINVAL;
	if (native_query && (port || iboot_probe || (frame_map && !native_frame_trial) ||
	    ((sleep_iop || restart_cpu) && !native_sleep_cycle) || (!native_route &&
	    (afk_endpoint || afk_query || link_training || link_capabilities))))
		return -EINVAL;
	if (restart_cpu && (!sleep_iop || port || afk_query != 7))
		return -EINVAL;
	if (link_training && (afk_query != 7 || port || !link_capabilities ||
	    ((sleep_iop || restart_cpu) && !native_sleep_cycle)))
		return -EINVAL;
	if (image_hold_ms < 1500 || image_hold_ms > 50000 ||
	    (image_hold_ms != 1500 && !iboot_image))
		return -EINVAL;
	if (iboot_image && (!iboot_probe || !frame_map))
		return -EINVAL;
	if (image_blank_cycle && (!iboot_image || image_hold_ms != 50000 || image_alternate))
		return -EINVAL;
	if (image_alternate && (!iboot_image || image_hold_ms != 50000 || frame_buffers != 2))
		return -EINVAL;
	if (iboot_close_after_route && (!iboot_probe || edid_probe || audio_probe))
		return -EINVAL;
	if (observe_slow_quiesce && (!iboot_image || image_hold_ms != 6000 ||
	    quiesce_with_route || quiesce_before_afk_stop || release_before_hpd_off))
		return -EINVAL;
	if (quiesce_with_power && !quiesce_with_route)
		return -EINVAL;
	if (quiesce_with_route && (!iboot_image || image_hold_ms != 6000 ||
	    iboot_close_after_route || release_before_hpd_off || quiesce_before_afk_stop))
		return -EINVAL;
	if (release_before_hpd_off && (afk_query != 7 || quiesce_before_afk_stop))
		return -EINVAL;
	if (quiesce_before_afk_stop && !native_route && (!iboot_close_after_route || !iboot_image))
		return -EINVAL;
	if (edid_probe && !native_route && (!iboot_probe || iboot_image))
		return -EINVAL;
	if (edid_probe && native_route && !native_mode_trial)
		return -EINVAL;
	if (audio_probe && (!iboot_probe || iboot_image))
		return -EINVAL;
	if (iboot_probe && !link_training)
		return -EINVAL;
	if (afk_query > 7 || (afk_query == 1 && afk_endpoint != 0x23) ||
	    (afk_query == 2 && (afk_endpoint != 0x24 || port != 0)) ||
	    (afk_query >= 3 && (afk_endpoint != 0x2a || port != 0)))
		return -EINVAL;
#endif
	if (port > 1 || PAGE_SIZE != LEASE_PAGE ||
	    !of_machine_is_compatible("apple,j713") ||
	    !of_machine_is_compatible("apple,t8132"))
		return -ENODEV;
	snprintf(alias, sizeof(alias), "dcpext%u", port);
	dcp = of_find_node_by_path(alias);
	if (!dcp || of_device_is_available(dcp) ||
	    !of_device_is_compatible(dcp, "apple,t8132-dcpext") ||
	    of_property_read_u32(dcp, "apple,j713-dcpext-reservations", &marker) || marker != 1 ||
	    of_property_read_string(dcp, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, "81BAF476-679D-3CC0-8C6A-E6B6FA659D5E"))
		goto done;
	if (!of_get_property(dcp, "apple,j713-dcpext-segments", &segment_bytes) ||
	    segment_bytes <= 0 || segment_bytes % 32)
		goto done;
	count = of_property_count_u32_elems(dcp, "memory-region");
	if (count != segment_bytes / 32 + 3 ||
	    of_property_read_u32_array(dcp, "iommus", params, 2) || params[1] != 23)
		goto done;
#ifdef DCPEXT_WITH_SESSION
	ret = session_prepare(dcp);
	if (ret)
		goto done;
	if (afk_query >= 6) {
		ret = session_atc_prepare(afk_query == 7);
		if (ret)
			goto done;
	}
	ret = -ENODEV;
#endif
	dart = of_find_node_by_phandle(params[0]);
	ram = of_parse_phandle(dcp, "memory-region", count - 3);
	dart_base = port ? 0x48a340000ULL : 0x286340000ULL;
	if (!dart || of_device_is_available(dart) ||
	    !of_device_is_compatible(dart, "apple,t8110-dart") ||
	    of_address_to_resource(dart, 0, &resource) || resource.start != dart_base ||
	    resource_size(&resource) != 0x20000 || !ram ||
	    !of_device_is_compatible(ram, "apple,dart-mem") ||
	    !of_property_read_bool(ram, "no-map") ||
	    of_address_to_resource(ram, 0, &reservation))
		goto done;
	ret = fabric_powered();
	if (ret)
		goto done;
	ret = -EBUSY;
	if (!request_mem_region(dart_base, 0x20000, "m4-dcpext-dma-probe"))
		goto done;
	region_owned = true;
	regs = ioremap(dart_base, 0x20000);
	ret = -ENOMEM;
	if (!regs)
		goto done;
	protect = readl(regs + 0x200);
	tcr = readl(regs + 0x1000 + 23 * 4);
	ttbr = readl(regs + 0x1400 + 23 * 4);
	root_pa = (u64)((ttbr & GENMASK(29, 2)) >> 2) << 14;
	ret = -EINVAL;
	if (!(protect & 1) || tcr != 1 || !(ttbr & 1) ||
	    root_pa < reservation.start || root_pa > reservation.end ||
	    reservation.end - root_pa + 1 < LEASE_PAGE || readl(regs + 0x100))
		goto done;
	root = memremap(root_pa, LEASE_PAGE, MEMREMAP_WB);
	ret = -ENOMEM;
	if (!root)
		goto done;
#ifdef DCPEXT_WITH_SESSION
	ret = session_tables(&reservation);
	if (ret)
		goto done;
	ret = -ENOMEM;
#endif
	saved = kmemdup(root, LEASE_PAGE, GFP_KERNEL);
	leaf = (void *)get_zeroed_page(GFP_KERNEL);
	if (!saved || !leaf)
		goto done;
	ret = -EBUSY;
	if (READ_ONCE(root[LEASE_SLOT]))
		goto done;
	dma_dev = root_device_register("m4-dcpext-dma-probe");
	if (IS_ERR(dma_dev)) {
		ret = PTR_ERR(dma_dev);
		dma_dev = NULL;
		goto done;
	}
	/* No OF node / IOMMU attachment: obtain a host-owned direct allocation.
	 * Require identity DMA-to-physical translation for the DART leaf.
	 */
	ret = dma_coerce_mask_and_coherent(dma_dev, DMA_BIT_MASK(42));
	if (ret)
		goto done;
	pool = dma_alloc_coherent(dma_dev, LEASE_SIZE, &pool_dma, GFP_KERNEL);
	ret = -ENOMEM;
	if (!pool)
		goto done;
	leaf_pa = virt_to_phys(leaf);
	ret = -EINVAL;
	if ((pool_dma | leaf_pa) & (LEASE_PAGE - 1) ||
	    dma_to_phys(dma_dev, pool_dma) != pool_dma ||
	    pool_dma + LEASE_SIZE > BIT_ULL(42) || leaf_pa >= BIT_ULL(42))
		goto done;
	for (i = 0; i < LEASE_SIZE / LEASE_PAGE; i++)
		((u64 *)leaf)[i] = encode_address(pool_dma + i * LEASE_PAGE) | PTE_RW;
	ret = pio_prepare(dcp, count);
	if (ret)
		goto done;
	ret = frame_prepare(dcp, count);
	if (ret)
		goto done;
	memset(pool, 0xa5, LEASE_SIZE);
	dma_wmb();
	entry = encode_address(leaf_pa) | 1;
	ret = -EAGAIN;
	if (readl(regs + 0x1400 + 23 * 4) != ttbr ||
	    readl(regs + 0x1000 + 23 * 4) != tcr ||
	    readl(regs + 0x200) != protect || memcmp(root, saved, LEASE_PAGE))
		goto done;
	/* A published table must survive failed invalidation/teardown. */
	__module_get(THIS_MODULE);
	pinned = true;
	if (cmpxchg64_relaxed(&root[LEASE_SLOT], 0, entry)) {
		module_put(THIS_MODULE);
		pinned = false;
		goto done;
	}
	ret = flush_sid();
	if (ret)
		goto done;
	ret = pio_publish();
	if (ret)
		goto done;
	ret = frame_publish();
	if (ret)
		goto done;
#ifdef DCPEXT_WITH_SESSION
	if (native_kms_trial) {
		struct task_struct *worker = kthread_run(session_kms_worker, NULL, "dcpext-kms");
		ret = IS_ERR(worker) ? PTR_ERR(worker) : 0;
		native_kms_worker_started = !ret;
		goto done;
	}
	ret = session_capture_data(false);
	if (ret)
		goto done;
	ret = session_run();
	if (capture_power_state) {
		int capture_ret = session_capture_data(true);

		pr_info("m4_dcpext_session: DATA final capture result=%d\n", capture_ret);
		if (!ret)
			ret = capture_ret;
	}
	if (ret)
		goto done;
#endif
#ifdef DCPEXT_WITH_SESSION
	pr_info("m4_dcpext_session: firmware quiesced; removing private mapping\n");
#else
	pr_info("m4_dcpext_dma_probe: port=%u SID23 private DVA=%#llx bytes=%#x mapped; no firmware notification\n",
		port, LEASE_DVA, LEASE_SIZE);
#endif
	ret = remove_private_mapping();
	if (ret)
		goto done;
	module_put(THIS_MODULE);
	pinned = false;
	pr_info("m4_dcpext_dma_probe: private mapping removed; inherited root unchanged\n");
done:
	of_node_put(ram);
	of_node_put(dart);
	of_node_put(dcp);
	result = ret;
	if (pinned) {
#ifdef DCPEXT_WITH_SESSION
		session_evidence_publish();
		if (native_kms_trial && !ret)
			return 0;
#endif
		pr_err("m4_dcpext_dma_probe: incomplete teardown (%d); retaining all allocations/resources until recovery\n", ret);
		return 0;
	}
	cleanup();
#ifdef DCPEXT_WITH_SESSION
	if (!ret)
		ret = session_evidence_publish();
	if (ret)
		session_evidence_cleanup();
#endif
	return ret;
}

#if defined(DCPEXT_DESKTOP) && !defined(MODULE)
#include "dcpext_boot.h"
#else
static void __exit probe_exit(void)
{
#ifdef DCPEXT_WITH_SESSION
	session_evidence_cleanup();
#endif
}
module_init(probe_init);
module_exit(probe_exit);
#endif
MODULE_LICENSE("Dual MIT/GPL");
#ifdef DCPEXT_WITH_SESSION
MODULE_DESCRIPTION("J713 external RTKit system boot/quiesce with additive DART mapping");
#else
MODULE_DESCRIPTION("J713 external DART additive mapping/remove diagnostic, no firmware execution");
#endif
