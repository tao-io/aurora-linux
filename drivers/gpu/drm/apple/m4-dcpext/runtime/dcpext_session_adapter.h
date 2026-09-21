/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Private adapter included by the shared mapping diagnostic implementation.
 * Mailbox: Asahi drivers/soc/apple/mailbox.c, fba91e5d9577; the earlier
 * Eryk Wieliczko probe f397d74d718e supplies the register-access pattern only.
 * Buffer admission: J713 m1n1 ab71c64 metadata and m4_dcp_rtkit_main.c.
 * External application/PHY operations are explicitly opt-in below. Native
 * binary behavior is a reference; no Apple machine code is imported.
 */
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include "dcpext_session.h"
#include "dcpext_syslog.h"
#include "dcpext_quiesce_dump.h"

static struct dcpext_session session;
static struct dcpext_syslog session_syslog;
static bool crash_on_quiesce_stall;
module_param(crash_on_quiesce_stall, bool, 0400);
static struct dcpext_quiesce_dump quiesce_dump;
static bool sleep_iop;
module_param(sleep_iop, bool, 0400);
MODULE_PARM_DESC(sleep_iop, "Experimental Linux RTKit restartable sleep after AP quiescence");
static bool restart_cpu;
module_param(restart_cpu, bool, 0400);
static bool native_cpu_stopped;
module_param(native_cpu_stopped, bool, 0400);
MODULE_PARM_DESC(restart_cpu, "External CPU RUN lifecycle, only with acknowledged IOP sleep; no reset/vector changes");
static void __iomem *session_cpu, *session_fifo;
static bool session_cpu_owned, session_fifo_owned;
static phys_addr_t session_base;
static struct resource session_table_ram;
static void *session_table_map, *session_table_saved;
static size_t session_used;
static u8 session_crashlog[0x100000];
static u32 session_crashlog_size;
static struct {
	struct resource ram;
	u64 remap;
	u32 size, flags;
} session_segments[32];
static unsigned int session_nsegments;
static bool capture_power_state;
module_param(capture_power_state, bool, 0400);
MODULE_PARM_DESC(capture_power_state, "Read-only DATA snapshots through verified SID23 mappings, before and after the bounded session");
static void *session_data_before, *session_data_after;
static u32 session_data_size;
static void session_atc_cleanup(void);
static void native_cleanup(void);

static void session_cleanup(void)
{
	native_cleanup();
	session_atc_cleanup();
	if (session_table_map)
		memunmap(session_table_map);
	kfree(session_table_saved);
	if (session_fifo)
		iounmap(session_fifo);
	if (session_cpu)
		iounmap(session_cpu);
	if (session_fifo_owned)
		release_mem_region(session_base + 0x8000, 0x4000);
	if (session_cpu_owned)
		release_mem_region(session_base, 0x4000);
}

static int session_prepare(struct device_node *dcp)
{
	struct device_node *node;
	struct resource res;
	const u8 *metadata;
	struct regmap *map;
	u32 power;
	int bytes, ret;

	session_base = port ? 0x48ae00000ULL : 0x286e00000ULL;
	if (of_address_to_resource(dcp, 0, &res) || res.start != session_base ||
	    resource_size(&res) != 0x4000)
		return -ENODEV;
	metadata = of_get_property(dcp, "apple,j713-dcpext-segments", &bytes);
	if (!metadata || bytes <= 0 || bytes % 32 || bytes / 32 > 32)
		return -EINVAL;
	session_nsegments = bytes / 32;
	for (unsigned int i = 0; i < session_nsegments; i++) {
		node = of_parse_phandle(dcp, "memory-region", i);
		ret = -EINVAL;
		if (node && of_device_is_available(node) &&
		    of_property_read_bool(node, "no-map") &&
		    of_device_is_compatible(node, "apple,asc-mem"))
			ret = of_address_to_resource(node, 0, &session_segments[i].ram);
		of_node_put(node);
		if (ret)
			return ret;
		session_segments[i].remap = get_unaligned_be64(metadata + i * 32 + 16);
		session_segments[i].size = get_unaligned_be32(metadata + i * 32 + 24);
		session_segments[i].flags = get_unaligned_be32(metadata + i * 32 + 28);
		if (!session_segments[i].size ||
		    session_segments[i].ram.start != get_unaligned_be64(metadata + i * 32) ||
		    resource_size(&session_segments[i].ram) != ALIGN((u64)session_segments[i].size, LEASE_PAGE))
			return -EINVAL;
	}
	/* No CPU control writes. SYS/FE checks are in the shared mapper. */
	node = of_find_node_by_path("/soc/power-management@380700000");
	if (!node)
		return -ENODEV;
	map = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(map))
		return PTR_ERR(map);
	ret = regmap_read(map, port ? 0x4f0 : 0x1e8, &power);
	if (ret)
		return ret;
	if ((power & 15) != 15 || power & (BIT(31) | BIT(12) | BIT(11) | BIT(10)))
		return -EHOSTDOWN;
	if (!request_mem_region(session_base, 0x4000, "m4-dcpext-session"))
		return -EBUSY;
	session_cpu_owned = true;
	if (!request_mem_region(session_base + 0x8000, 0x4000, "m4-dcpext-session"))
		return -EBUSY;
	session_fifo_owned = true;
	/* Mapping here does not touch the registers; fabric is checked before
	 * session_run accesses these windows.
	 */
	session_cpu = ioremap(session_base, 0x4000);
	session_fifo = ioremap(session_base + 0x8000, 0x4000);
	return session_cpu && session_fifo ? 0 : -ENOMEM;
}

static int session_tables(const struct resource *reservation)
{
	session_table_ram = *reservation;
	if (!resource_size(reservation) || resource_size(reservation) > 0x100000)
		return -EINVAL;
	session_table_map = memremap(reservation->start, resource_size(reservation), MEMREMAP_WB);
	if (!session_table_map)
		return -ENOMEM;
	session_table_saved = kmemdup(session_table_map, resource_size(reservation), GFP_KERNEL);
	return session_table_saved ? 0 : -ENOMEM;
}

static int session_translate(u64 wire, phys_addr_t *physical)
{
	u64 dva, entry, leaf_pa, pte;
	u64 *table;

	if ((wire >> 36) != 0x10)
		return -ERANGE;
	dva = wire & GENMASK_ULL(35, 0);
	entry = READ_ONCE(root[dva >> 25]);
	if (!(entry & 1))
		return -ENOENT;
	leaf_pa = (entry & PTE_ADDRESS) << 4;
	if (leaf_pa < session_table_ram.start || leaf_pa > session_table_ram.end ||
	    session_table_ram.end - leaf_pa + 1 < LEASE_PAGE)
		return -ERANGE;
	table = session_table_map + leaf_pa - session_table_ram.start;
	pte = READ_ONCE(table[(dva >> 14) & 2047]);
	if (!(pte & 1))
		return -ENOENT;
	*physical = ((pte & PTE_ADDRESS) << 4) | (dva & (LEASE_PAGE - 1));
	return 0;
}

static int session_capture_data(bool after)
{
	void **snapshot = after ? &session_data_after : &session_data_before;
	void *mapping;
	phys_addr_t translated;
	u32 size;
	int ret;

	if (!capture_power_state)
		return 0;
	/* The J713 DATA reservation is validated in session_prepare. Verify
	 * every active DART translation before reading; never follow arbitrary
	 * firmware pointers or map addresses obtained from an untrusted PTE.
	 */
	if (port || session_nsegments != 4 || session_segments[1].flags ||
	    session_segments[1].size != 0x325000 || *snapshot)
		return -EINVAL;
	size = session_segments[1].size;
	for (u32 offset = 0; offset < size; offset += LEASE_PAGE) {
		ret = session_translate(session_segments[1].remap + offset, &translated);
		if (ret || translated != session_segments[1].ram.start + offset)
			return ret ? ret : -ESTALE;
	}
	*snapshot = vzalloc(size);
	if (!*snapshot)
		return -ENOMEM;
	mapping = memremap(session_segments[1].ram.start, size, MEMREMAP_WB);
	if (!mapping) {
		vfree(*snapshot);
		*snapshot = NULL;
		return -ENOMEM;
	}
	dma_rmb();
	memcpy(*snapshot, mapping, size);
	memunmap(mapping);
	session_data_size = size;
	pr_info("m4_dcpext_session: DATA %s snapshot bytes=%#x; every SID23 page matches reservation, cached sequential read\n",
		after ? "after" : "before", size);
	return 0;
}

static int session_buffer(void *cookie, u8 ep, u64 requested, u32 size, u64 *dva)
{
	phys_addr_t pa, checked;
	size_t rounded = ALIGN(size, LEASE_PAGE);
	int ret;

	if (!requested) {
		if (rounded > LEASE_SIZE - session_used)
			return -ENOSPC;
		memset(pool + session_used, 0, rounded);
		*dva = BIT_ULL(40) | (LEASE_DVA + session_used);
		session_used += rounded;
		pr_info("m4_dcpext_session: ep=%u host buffer size=%#x dva=%#llx\n", ep, size, *dva);
		return 0;
	}
	for (unsigned int i = 0; i < session_nsegments; i++) {
		/* ADT flags 0xa OS-log region is a host physical carveout, not
		 * a DART mapping. Exact base and declared size are mandatory.
		 */
		if (ep == 8 && session_segments[i].flags == 0xa &&
		    requested == session_segments[i].ram.start &&
		    requested == session_segments[i].remap && size <= session_segments[i].size) {
			*dva = requested;
			pr_info("m4_dcpext_session: ep=8 reserved physical OSLog size=%#x address=%#llx\n", size, requested);
			return 0;
		}
	}
	ret = session_translate(requested, &pa);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < session_nsegments; i++) {
		struct resource *r = &session_segments[i].ram;
		if (session_segments[i].flags == 0xa || pa < r->start || pa > r->end ||
		    size - 1 > r->end - pa)
			continue;
		/* Verify every page boundary, including an unaligned first page. */
		for (u64 off = 0; off < size; ) {
			ret = session_translate(requested + off, &checked);
			if (ret || checked != pa + off)
				return -ERANGE;
			off += LEASE_PAGE - ((requested + off) & (LEASE_PAGE - 1));
		}
		ret = session_translate(requested + size - 1, &checked);
		if (ret || checked != pa + size - 1)
			return -ERANGE;
		*dva = requested;
		pr_info("m4_dcpext_session: ep=%u inherited size=%#x dva=%#llx physical=%#llx\n", ep, size, requested, (u64)pa);
		return 0;
	}
	return -ERANGE;
}

static u64 session_now(void *cookie)
{
	return ktime_get_boottime_ns() / NSEC_PER_MSEC;
}

static int session_capture_crashlog(u64 message)
{
	const struct dcpext_buffer *b = &session.buffers[1];
	u64 requested = message & ((1ULL << 44) - 1), checked;
	u32 size = b->size;
	phys_addr_t pa;
	void *mapping;
	int ret;

	if ((message >> 52) != 1 || !b->ready)
		return 0; /* Initial crash-buffer registration is handled normally. */
	/* The second notification's size field is not the allocation size
	 * (the retained M4 crash reports 3 pages for a 16-page buffer).
	 * Like Linux RTKit, copy only the previously admitted allocation.
	 */
	if (requested != b->dva || !size ||
	    size > sizeof(session_crashlog))
		return -ERANGE;
	if (!b->inherited) {
		u64 base = BIT_ULL(40) | LEASE_DVA;
		u64 offset = b->dva - base;

		if (b->dva < base || offset > session_used || size > session_used - offset)
			return -ERANGE;
		dma_rmb();
		memcpy(session_crashlog, pool + offset, size);
	} else {
		/* Reuse the full reservation and page-by-page translation admission.
		 * No address from the new crash notification is trusted by itself.
		 */
		ret = session_buffer(NULL, 1, b->dva, size, &checked);
		if (ret || checked != b->dva)
			return ret ?: -ERANGE;
		ret = session_translate(b->dva, &pa);
		if (ret)
			return ret;
		mapping = memremap(pa, size, MEMREMAP_WB);
		if (!mapping)
			return -ENOMEM;
		dma_rmb();
		memcpy(session_crashlog, mapping, size);
		memunmap(mapping);
	}
	smp_store_release(&session_crashlog_size, size);
	pr_err("m4_dcpext_session: captured %u bytes of firmware crashlog; recovery required\n", size);
	return 0;
}

static int session_send(void *cookie, u8 ep, u64 message)
{
	u32 status;
	int ret = readl_poll_timeout(session_fifo + 0x110, status, !(status & BIT(16)), 100, 100000);
	if (ret)
		return ret;
	dma_wmb();
	writeq(message, session_fifo + 0x800);
	writeq(ep, session_fifo + 0x808);
	if (!desktop || !session.runtime)
		pr_info("m4_dcpext_session: TX ep=%#x msg=%#llx\n", ep, message);
	return 0;
}

static int session_capture_syslog(u64 message)
{
	const struct dcpext_buffer *b = &session.buffers[2];
	u64 offset, base = BIT_ULL(40) | LEASE_DVA;
	u32 type = (message >> 52) & 0xff;
	if (type != 5)
		return dcpext_syslog_observe(&session_syslog, message, NULL, 0);
	/* Accept only our zeroed, published host allocation. Never translate
	 * an inherited firmware address just to collect diagnostic text.
	 */
	if (!b->ready || b->inherited || b->dva < base)
		return -EINVAL;
	offset = b->dva - base;
	if (offset > session_used || b->size > session_used - offset)
		return -ERANGE;
	dma_rmb();
	if (desktop && session_syslog.used > sizeof(session_syslog.capture) - 512)
		session_syslog.used = 0;
	return dcpext_syslog_observe(&session_syslog, message, pool + offset, b->size);
}

#include "dcpext_afk_probe.h"
static int session_dispatch(u64 endpoint, u64 message);
#include "dcpext_native_adapter.h"

static int session_stop_display_route(void)
{
	int ret;
	if (session_dpxbar_selected) {
		if (afk_route_dirty || afk_display_dirty || afk_hpd_dirty || session_atc_fault)
			return -EUCLEAN;
		if (link_training && session_link.rate) {
			ret = dptx_link_set_rate(&session_link, 0);
			if (ret)
				return ret;
		}
		ret = dpxbar_down();
		if (ret)
			return ret;
	}
	return 0;
}

static int session_dispatch(u64 endpoint, u64 message)
{
	int ret;
	dma_rmb();
	ret = dcpext_session_runtime_message(&session);
	if (ret)
		return ret;
	if (!desktop || !session.runtime)
		pr_info("m4_dcpext_session: RX ep=%#llx msg=%#llx\n", endpoint, message);
	if ((endpoint & 0xff) == 1) {
		ret = session_capture_crashlog(message);
		if (ret)
			return ret;
	}
	if ((endpoint & 0xff) == 2) {
		ret = session_capture_syslog(message);
		if (ret)
			return ret;
	}
	if (native_query && (endpoint & 0xff) == 0x37)
		ret = native_receive(message);
	else if ((audio_probe || audio_desktop) && (endpoint & 0xff) == 0x29 && audio_channel.started)
		ret = audio_adapter_receive(message);
	else if (edid_probe && (endpoint & 0xff) == 0x28 && edid_channel.started)
		ret = edid_adapter_receive(message);
	else if (iboot_probe && (endpoint & 0xff) == 0x23 && iboot_channel.started)
		ret = iboot_adapter_receive(message);
	else if (afk_endpoint && (endpoint & 0xff) == afk_endpoint && afk_probe_started)
		ret = afk_probe_handle(message);
	else
		ret = dcpext_session_receive(&session, endpoint & 0xff, message);
	return ret;
}

static int session_run_protocol(void)
{
	static const struct dcpext_session_ops ops = {
		.now_ms = session_now, .send = session_send, .buffer = session_buffer,
	};
	u64 message, endpoint;
	u32 state;
	int ret;

	state = readl(session_cpu + 0x44);
	if ((state != 0x10 && !(restart_cpu && sleep_iop && state == 0)) ||
	    !(readl(session_fifo + 0x114) & BIT(17)))
		return -EBUSY;
	if (restart_cpu && !sleep_iop)
		return -EINVAL;
	if (!state) {
		/* Asahi dcp.c fresh boot / m1n1 asc_cpu_start: set RUN only.
		 * UUID, reservations, private DART mapping and exclusive external
		 * register ownership were already qualified by the caller.
		 */
		writel(BIT(4), session_cpu + 0x44);
		if (readl(session_cpu + 0x44) != BIT(4))
			return -EIO;
		pr_info("m4_dcpext_session: external CPU RUN set; inherited firmware vector unchanged\n");
	}
	session.sleep_iop = sleep_iop;
	session.defer_iop_quiesce = quiesce_before_afk_stop;
	if (iboot_image && image_hold_ms > 1500)
		session.duration_ms = image_hold_ms > 6000 ? 60000 : 16000;
	/* Diagnostic only: leave ~35 seconds after the six-second image.
	 * No watchdog renewal; the 45-second absolute deadline leaves evidence
	 * collection time before the unchanged 60-second watchdog expires. */
	if (observe_slow_quiesce)
		session.duration_ms = 45000;
	if (native_startup)
		session.duration_ms = 16000;
	/* Forty surface updates add legitimate firmware log messages. Retain a
	 * fixed count bound and the unchanged absolute/thermal/watchdog gates. */
	if (image_alternate || image_blank_cycle)
		session.message_limit = 512;
	if (native_route && edid_probe)
		session.message_limit = 512; /* Extra AV endpoint, same 16-second deadline. */
	if (native_kms_trial)
		session.message_limit = 8192;
	native_route_pending = native_mode_trial;
	ret = dcpext_session_begin(&session, &ops, NULL);
	while (!ret && session.phase != DCPEXT_QUIESCED) {
		if (desktop && native_kms_ready && !native_kms_finished) {
			ret = native_kms_hotplug_tick();
			if (ret)
				break;
		}
		if (native_kms_trial && native_kms_ready &&
		    (((!desktop || desktop_test_ms) && session_now(NULL) >= native_kms_until) ||
		     READ_ONCE(desktop_stop)))
			native_kms_unplug();
		guard(mutex)(&native_route_lock);
		ret = dcpext_session_poll(&session);
		if (ret)
			break;
		if (crash_on_quiesce_stall) {
			ret = dcpext_quiesce_dump_tick(&quiesce_dump,
				session.phase == DCPEXT_AP_QUIESCE && !session.ap_quiesced,
				session_now(NULL), session.deadline);
			if (ret < 0)
				break;
			if (ret) {
				pr_warn("m4_dcpext_session: diagnostic EP1 soft crash after 5s without AP ACK; retain resources for recovery\n");
				ret = session_send(NULL, 1, 0x40);
				if (ret)
					break;
			}
		}
		if (session.phase == DCPEXT_RUNNING ||
		    (session.phase == DCPEXT_AP_QUIESCE && session.ap_quiesced)) {
			ret = native_progress();
			if (!ret)
				ret = afk_probe_progress();
			if (ret)
				break;
			if (quiesce_before_afk_stop && session.phase == DCPEXT_RUNNING &&
			    dptx_trial_phase == DPTX_TRIAL_DONE && (native_route || iboot_ready_for_route_release)) {
				if (afk_route_dirty || afk_display_dirty || afk_hpd_dirty ||
				    session_atc_fault || iboot_state.power_dirty)
					return -EUCLEAN;
				ret = native_stop();
				if (!ret)
					ret = session_stop_display_route();
				if (ret)
					return ret;
				pr_info("m4_dcpext_session: requesting AP quiescence with AFK channels open\n");
				ret = dcpext_session_quiesce(&session);
				if (ret)
					break;
			}
			if ((!native_query || native_state.phase == 3) &&
			    (!native_startup || native_startup_done) &&
			    (!afk_endpoint || afk_probe_stopped) &&
			    (!audio_probe || audio_finished) && (!edid_probe || edid_finished) &&
			    (!quiesce_before_afk_stop || native_route || iboot_finished)) {
				if (iboot_probe && (!iboot_finished || (iboot_state.power_dirty && !quiesce_with_power)))
					return -EUCLEAN;
				/* Native A457 opened a client even when no surface was
				 * submitted. Pair it with A458 and the native power-off
				 * request before removing clocks or requesting AP sleep. */
				ret = native_stop();
				if (ret)
					return ret;
				/* Display release + disconnect + AFK shutdown establish
				 * that the link is idle. Stop the external crossbar before
				 * asking DCP to sleep; leaving its source clocks on can
				 * prevent the AP-quiescence acknowledgement.
				 * DART/PHY resources still remain pinned until both ACKs.
				 */
				ret = quiesce_with_route ? 0 : session_stop_display_route();
				if (ret)
					return ret;
				pr_info("m4_dcpext_session: requesting AP then IOP quiescence\n");
				ret = quiesce_before_afk_stop ? dcpext_session_finish_quiesce(&session) :
					dcpext_session_quiesce(&session);
				if (ret)
					break;
			}
		}
		if (native_startup_done) {
			ret = native_drain();
			if (ret)
				break;
		}
		state = readl(session_fifo + 0x114);
		if (state & BIT(17)) {
			usleep_range(100, 200);
			continue;
		}
		message = readq(session_fifo + 0x830);
		endpoint = readq(session_fifo + 0x838);
		ret = session_dispatch(endpoint, message);
	}
	native_kms_unplug();
	if (quiesce_dump.sent && !ret)
		ret = -ECANCELED;
	if (!ret) {
		/* No new traffic is expected after both ACKs. Drain means empty,
		 * never discarding unhandled replies and then freeing buffers.
		 */
		usleep_range(1000, 2000);
		if (!(readl(session_fifo + 0x114) & BIT(17)))
			ret = -EPROTO;
		if (!ret && restart_cpu) {
			if (afk_route_dirty || afk_display_dirty || afk_hpd_dirty || session_atc_fault)
				return -EUCLEAN;
			/* Both AP-quiesced and IOP-sleep ACKs precede stop. Native
			 * AppleASCWrapV6::_runCPU(false), as in Asahi dcp.c: clear
			 * RUN, reread, clear bit5. Never stop a failed live session.
			 */
			state = readl(session_cpu + 0x44);
			if (state != 0x10)
				return -EIO;
			writel(state & ~BIT(4), session_cpu + 0x44);
			state = readl(session_cpu + 0x44);
			writel(state & ~BIT(5), session_cpu + 0x44);
			if (readl(session_cpu + 0x44))
				return -EIO;
			native_cpu_stopped = true;
			pr_info("m4_dcpext_session: external CPU stopped after acknowledged sleep\n");
		}
	}
	if (afk_route_dirty || afk_display_dirty || afk_hpd_dirty || session_atc_fault ||
	    (iboot_probe && iboot_state.power_dirty)) {
		pr_err("m4_dcpext_session: DPTX cleanup incomplete route=%u display=%u hpd=%u phy_fault=%u; retain resources for watchdog recovery\n",
		       afk_route_dirty, afk_display_dirty, afk_hpd_dirty, session_atc_fault);
		ret = ret ? ret : -EUCLEAN;
	}
	pr_info("m4_dcpext_session: phase=%u ret=%d host_bytes=%zu received=%u\n",
		session.phase, ret, session_used, session.received);
	if (capture_power_state)
		pr_info("m4_dcpext_session: final mailbox A2I=%#x I2A=%#x CPU_RUN=%#x\n",
			readl(session_fifo + 0x110), readl(session_fifo + 0x114),
			readl(session_cpu + 0x44));
	return ret;
}

static int session_run(void)
{
	int ret = afk_query >= 6 ? session_atc_run(session_run_protocol, afk_query == 7) : session_run_protocol();

	if (native_query) {
		native_sys_error = readl(regs + 0x100);
		native_pio_error = pio_regs ? readl(pio_regs + 0x100) : 0;
		pr_info("m4_dcpext_native: final SYS error=%#x PIO error=%#x\n", native_sys_error, native_pio_error);
		if (!ret && (native_sys_error || native_pio_error))
			ret = -EIO;
	}
	return ret;
}

/* Async module startup lets DRM userspace open the card after module init.
 * Unplug severs owner callbacks before protocol teardown. Drop the final
 * execution pin only through module_put_and_kthread_exit, after all evidence
 * and ownership state are published. Errors retain every uncertain resource. */
static bool native_kms_worker_done;
static bool native_kms_worker_started;
static DECLARE_COMPLETION(native_kms_worker_exited);
module_param(native_kms_worker_done, bool, 0400);
static int session_kms_worker(void *unused)
{
	int ret;

#ifdef MODULE
	while (READ_ONCE(THIS_MODULE->state) == MODULE_STATE_COMING)
		usleep_range(1000, 2000);
#endif
	ret = session_run();
	if (!ret)
		ret = remove_private_mapping();
	if (!ret)
		cleanup();
	WRITE_ONCE(result, ret);
	pr_info("m4_dcpext_native: bounded KMS worker ended %d, %u completed frames; resources %s\n",
		ret, native_kms_frames, ret ? "retained" : "released");
	WRITE_ONCE(native_kms_worker_done, true);
	complete_all(&native_kms_worker_exited);
	if (!ret) {
		pinned = false;
		module_put_and_kthread_exit(0);
	}
	return 0;
}
