/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Selected J713 ATC ownership for the external-DCP runtime.
 * Reuses the independently hardware-qualified ATC component implementation.
 * HPD selects the negotiated pin-D orientation and corresponding lane pair.
 * No USB2, AXI lock or clock-provider changes are performed.
 */
#include <linux/iopoll.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-apple-t8132.h>
#include <linux/mutex.h>
#include <linux/spmi.h>
#include <linux/workqueue.h>
#include "tps6598x.h"
#include <linux/usb/typec_cd321x.h>
#include <linux/usb/typec_dp.h>
static void __iomem *atc_core;
static struct phy *session_shared_phy;
static bool session_shared_power, session_shared_initialized;
static bool atc_pinned, session_atc_owned, session_atc_active, session_atc_fault;
static bool session_atc_hpd;
static u64 session_atc_hpd_irqs;
static bool session_atc_flipped = true;
module_param_named(usb_c_flipped, session_atc_flipped, bool, 0400);
static bool session_atc_partner_flipped;
static struct regmap *session_atc_pmgr;
static u32 session_atc_power;
/* The controller is independent of the physical USB-C port. Prefer distinct
 * ports at boot; exclusive claims also cover concurrent startup and migration.
 */
static unsigned int session_atc_port = 1 - DCPEXT_INSTANCE;
module_param_named(usb_c_phy, session_atc_port, uint, 0400);
static phys_addr_t session_atc_base(void)
{
	return 0x403000000ULL + session_atc_port * 0x8000000ULL;
}
static u32 session_atc_power_offset(void)
{
	return session_atc_port ? 0x428 : 0x378;
}
static u32 session_atc_target(void)
{
	/* Asahi dptxep.h: connected bit15, ATC number in bits7:4. */
	return 0x8000 | (session_atc_port << 4);
}
#include "dcpext_atc_power.h"
#include "dcpext_atc_common.h"
#include "dcpext_dpxbar.h"
#include "dcpext_atc_dp_main.h"
#include "dcpext_atc_dp_drive.h"
#include "dcpext_atc_main_snapshot.h"
#include "dcpext_dptx_link.h"
static bool link_training;
module_param(link_training, bool, 0400);
MODULE_PARM_DESC(link_training, "Guarded HBR2/two-lane/native-fallback-drive training trial; no scanout");
static struct dptx_link_state session_link;
static int (*session_atc_protocol)(void);

static int session_atc_partner_at(unsigned int phy, bool report, bool *hpd, bool *flipped)
{
	/* Owner-serialized TIPD state, refreshed by controller interrupts; register definitions are Intel/Asahi tps6598x.h.
	 * The current lane policy admits pin-D on either J713 port.
	 */
	struct device_node *node;
	struct spmi_device *sdev;
	struct cd321x_dp_snapshot snapshot;
	int (*get_snapshot)(struct device *, struct cd321x_dp_snapshot *);
	u32 status = 0, data = 0;
	int ret = -ENODEV;
	*hpd = false;
	*flipped = false;
	for_each_compatible_node(node, NULL, "apple,t8132-sn201202x") {
		sdev = spmi_find_device_by_of_node(node);
		if (!sdev)
			continue;
		if (strcmp(dev_name(&sdev->dev), phy ? "1-0a" : "1-0c")) {
			spmi_device_put(sdev);
			continue;
		}
		/* Refuse old kernels: a sticky cached bit cannot substitute for an
		 * event counter. symbol_get keeps standalone diagnostic builds
		 * usable without linking against a different kernel's symvers. */
		get_snapshot = symbol_get(cd321x_get_dp_state);
		if (!get_snapshot) {
			ret = -EOPNOTSUPP;
		} else {
			ret = get_snapshot(&sdev->dev, &snapshot);
			symbol_put(cd321x_get_dp_state);
		}
		if (!ret) {
			status = snapshot.status;
			data = snapshot.data_status;
			if (phy == session_atc_port)
				session_atc_hpd_irqs = snapshot.hpd_irqs;
			*flipped = !!(status & TPS_STATUS_PLUG_UPSIDE_DOWN);
			*hpd = !!(snapshot.dp_status & DP_STATUS_HPD_STATE);
			ret = status & TPS_STATUS_PLUG_PRESENT &&
			      data & TPS_DATA_STATUS_DP_CONNECTION &&
			      TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT(data) == TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_D ? 0 : -ENOLINK;
		}
		spmi_device_put(sdev);
		of_node_put(node);
		if (report)
			pr_info("m4_dcpext_session: ATC%u cached Type-C status=%#x data=%#x pin-D admission=%d\n",
				phy, status, data, ret);
		return ret;
	}
	return ret;
}

static int session_atc_read_partner(bool report)
{
	int ret = session_atc_partner_at(session_atc_port, report, &session_atc_hpd,
					 &session_atc_partner_flipped);
	/* An orientation change needs full PHY release/reconfiguration even
	 * on the same port; never drive a newly attached cable with old lanes. */
	if (!ret && session_atc_owned && session_atc_partner_flipped != session_atc_flipped)
		session_atc_hpd = false;
	return ret;
}

/* Admission must wait for Type-C mux completion before any firmware setup.
 * This is a software-state check; taking a PHY reference does not power it. */
static int session_atc_provider_ready(unsigned int port, bool flipped)
{
	struct device_node *node = of_find_node_by_path("/soc/dcp@286e00000");
	struct phy *phy;
	int ret;

	if (!node || of_property_match_string(node, "phy-names", "dp-phy0") < 0) {
		of_node_put(node);
		return 0;
	}
	phy = of_phy_get(node, port ? "dp-phy1" : "dp-phy0");
	of_node_put(node);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	ret = apple_t8132_dp_check_route(phy, flipped);
	of_phy_put(phy);
	return ret;
}

static int session_atc_find_partner(void)
{
	bool hpd, flipped;
	int ret, deferred = -ENOLINK;

	for (unsigned int i = 0; i < 2; i++) {
		unsigned int phy = session_atc_port ^ i;

		ret = session_atc_partner_at(phy, false, &hpd, &flipped);
		if (ret) {
			if (ret != -ENOLINK)
				deferred = ret;
			continue;
		}
		if (!hpd)
			continue;
		ret = session_atc_provider_ready(phy, flipped);
		if (ret) {
			deferred = ret;
			continue;
		}
#ifdef DCPEXT_DESKTOP
		if (!m4_dcpext_route_claim(port, phy))
			continue;
#endif
		return phy;
	}
	return deferred;
}

static int session_atc_check_partner(void)
{
	int ret = session_atc_read_partner(true);

	return !ret && session_atc_owned && session_atc_partner_flipped != session_atc_flipped ? -ENOLINK : ret;
}

static unsigned int session_atc_pair(void)
{
	return session_atc_flipped ? 0 : 1;
}

static u32 session_atc_main_offset(unsigned int index)
{
	u32 offset = atc_main_offsets[index];

	/* The native main-link list contains common/PLL plus pair0 registers.
	 * Pair1's lane bank is 0x7000 later; common/PLL addresses stay fixed. */
	return offset >= 0x9000 ? offset + session_atc_pair() * 0x7000 : offset;
}

static int dptx_link_set_rate(struct dptx_link_state *s, u32 rate)
{
	int ret;
	if (s->fault || session_atc_fault)
		return -EIO;
	if (rate != 0 && rate != 6 && rate != 0xa && rate != 0x14)
		return -EOPNOTSUPP;
	if (rate == s->rate)
		return 0;
	if (!atc_pinned || !session_atc_active ||
	    readl(atc_core + 0x20000) != 0x1b)
		return -EIO;
	/* Never remove the source clock before its crossbar has stopped.
	 * Normally callback5 already did this; also cover AUX/final cleanup.
	 */
	if (session_dpxbar_selected) {
		ret = dpxbar_down();
		if (ret) {
			s->fault = session_atc_fault = true;
			return ret;
		}
	}
	if (rate) {
		ret = session_atc_check_partner();
		if (ret)
			return ret;
		if ((readl(atc_core + 0x60) & 0xfff) != (session_atc_flipped ? 0x252 : 0x489) ||
		    (readl(atc_core + 0x64) & 0x1ffff) != (session_atc_flipped ? 0x111 : 0x110))
			return -EIO;
		/* A nonzero-to-nonzero rate change must retire the old PLL/lanes
		 * before running the native initialization sequence again. */
		ret = s->rate ? atc_dp_main_stop(BIT(session_atc_pair())) : 0;
		if (!ret)
			ret = atc_dp_main_prepare(BIT(session_atc_pair()), rate, false);
	} else {
		ret = atc_dp_main_stop(BIT(session_atc_pair()));
	}
	pr_info("m4_dcpext_session: main-link requested=%#x result=%d pll=%#x clocks=%#x\n",
		rate, ret, readl(atc_core + 0x7034), readl(atc_core + 0x7000));
	if (ret) {
		s->fault = session_atc_fault = true;
		return ret;
	}
	s->rate = rate;
	memset(s->drive, 0, sizeof(s->drive));
	return 0;
}

static int dptx_link_set_drive(struct dptx_link_state *s, const u32 drive[6])
{
	/* Native M4 getTrainingTableEntry fallback at image VA77951b0.
	 * Current captured ADT has no dp-training-table[-hbr] override.
	 * Express register facts independently; no native code is imported.
	 */
	static const u32 table[16] = {
		0x3f024, 0x237013, 0x37200b, 0x46e004,
		0x3f01c, 0x23700b, 0x372004, 0x3f024,
		0x3f013, 0x237004, 0x3f024, 0x3f024,
		0x3f004, 0x3f024, 0x3f024, 0x3f024,
	};
	u32 rx, tx;
	int ret;
	for (unsigned int i = 0; i < 6; i += 3)
		if (drive[i] || drive[i + 1] > 3 || drive[i + 2] > 3 ||
		    drive[i + 1] + drive[i + 2] > 3)
			return -EINVAL;
	if (!atc_pinned || !session_atc_active || (s->rate != 6 && s->rate != 0xa && s->rate != 0x14) ||
	    s->lanes != 2 || !(readl(atc_core + 0x7034) & 8))
		return -EIO;
	ret = session_atc_check_partner();
	if (ret)
		return ret;
	/* Native drive-change order: DP lane0 RX, lane1 TX of the selected pair. */
	rx = table[drive[1] * 4 + drive[2]];
	tx = table[drive[4] * 4 + drive[5]];
	ret = atc_dp_drive_preset(session_atc_pair(), false, rx);
	if (!ret)
		ret = atc_dp_drive_preset(session_atc_pair(), true, tx);
	pr_info("m4_dcpext_session: drive RX=%u/%u preset=%#x TX=%u/%u preset=%#x ret=%d\n",
		drive[1], drive[2], rx, drive[4], drive[5], tx, ret);
	return ret;
}

static int dptx_link_begin_config(struct dptx_link_state *s)
{
	if (!atc_pinned || !session_atc_active || s->fault || session_atc_fault)
		return -EIO;
	/* AppleATCDPPort slot a48 is takeConnectionDown, not a PHY stop.
	 * PHY rate remains valid until the subsequent setLinkRate request.
	 * Initial monitor admission also selected this crossbar at rate zero.
	 */
	return dpxbar_down();
}

static int dptx_link_validate_config(struct dptx_link_state *s)
{
	if (!atc_pinned || s->fault || session_atc_fault)
		return -EIO;
	if (!!(readl(atc_core + 0x7034) & 8) != !!s->rate)
		return -EIO;
	/* Native slot a50 is bringConnectionUp, guarded by cached rate624.
	 * Rate zero keeps the connection down. Final idle checks still apply.
	 */
	return s->rate && !session_dpxbar_selected ? dpxbar_up() : 0;
}

/* Release mappings separately from the route reservation: reconfiguring the
 * same PHY for a reversed plug must keep its controller as exclusive owner. */
static void session_atc_unmap(void)
{
	if (session_dpxbar)
		iounmap(session_dpxbar);
	session_dpxbar = NULL;
	if (session_dpxbar_owned)
		release_mem_region(session_atc_base() + 0x4c000, 0x4000);
	session_dpxbar_owned = false;
	if (session_shared_phy) {
		/* A failed consumer cannot drop an active shared PHY reference. */
		if (WARN_ON(session_shared_power))
			return;
		if (session_shared_initialized) {
			phy_exit(session_shared_phy);
			session_shared_initialized = false;
		}
		of_phy_put(session_shared_phy);
		session_shared_phy = NULL;
	} else {
		if (atc_core)
			iounmap(atc_core);
		if (session_atc_owned)
			release_mem_region(session_atc_base(), 0x24000);
	}
	atc_core = NULL;
	session_atc_owned = false;
}

static void session_atc_cleanup(void)
{
	session_atc_unmap();
#ifdef DCPEXT_DESKTOP
	m4_dcpext_route_release(port, session_atc_port);
#endif
}

static int session_atc_prepare(bool route_dpxbar)
{
	struct device_node *node;
	u32 cells[384];
	int count, ret;
#ifdef DCPEXT_DESKTOP
	if (!session.runtime) {
		ret = session_atc_find_partner();
		if (ret < 0)
			return ret;
		/* The boot admission may have reserved another PHY before its
		 * cable disappeared. No MMIO is owned yet; retire that reservation. */
		if (ret != session_atc_port)
			m4_dcpext_route_release(port, session_atc_port);
		session_atc_port = ret;
	}
#endif
	ret = session_atc_read_partner(true);
	if (ret)
		return ret;
	session_atc_flipped = session_atc_partner_flipped;
	node = of_find_node_by_path(session_atc_port ? "/soc/phy@40aa90000" : "/soc/phy@402a90000");
	if (!node)
		return -ENODEV;
	count = of_property_count_u32_elems(node, "apple,tunable-common-b");
	if (count <= 0 || count > ARRAY_SIZE(cells) || count % 3) {
		of_node_put(node);
		return -EINVAL;
	}
	ret = of_property_read_u32_array(node, "apple,tunable-common-b", cells, count);
	of_node_put(node);
	if (ret)
		return ret;
	ret = atc_component_select(cells, count);
	if (ret)
		return ret;
	node = of_find_node_by_path("/soc/power-management@380700000");
	if (!node)
		return -ENODEV;
	session_atc_pmgr = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(session_atc_pmgr))
		return PTR_ERR(session_atc_pmgr);
	ret = regmap_read(session_atc_pmgr, session_atc_power_offset(), &session_atc_power);
	if (ret)
		return ret;
	if ((session_atc_power & 0xff) != 0xff ||
	    session_atc_power & (BIT(31) | BIT(28) | BIT(12) | BIT(11) | BIT(10)))
		return -EHOSTDOWN;
	/* New handoffs give both display controllers access to both provider
	 * PHYs. The provider retains common reset/power while USB is using it. */
	node = of_find_node_by_path("/soc/dcp@286e00000");
	if (node && of_property_match_string(node, "phy-names", "dp-phy0") >= 0) {
		session_shared_phy = of_phy_get(node, session_atc_port ? "dp-phy1" : "dp-phy0");
		of_node_put(node);
		if (IS_ERR(session_shared_phy)) {
			ret = PTR_ERR(session_shared_phy);
			session_shared_phy = NULL;
			return ret;
		}
		ret = apple_t8132_dp_check_route(session_shared_phy, session_atc_flipped);
		if (ret) {
			of_phy_put(session_shared_phy);
			session_shared_phy = NULL;
			return ret;
		}
		ret = phy_init(session_shared_phy);
		if (ret)
			return ret;
		session_shared_initialized = true;
		ret = phy_power_on(session_shared_phy);
		if (ret)
			return ret;
		session_shared_power = true;
		/* Recheck after acquiring power: the reference now prevents a
		 * mux change between orientation validation and DP lane access. */
		ret = apple_t8132_dp_check_route(session_shared_phy, session_atc_flipped);
		if (ret) {
			int off = phy_power_off(session_shared_phy);

			if (off)
				return off;
			session_shared_power = false;
			phy_exit(session_shared_phy);
			session_shared_initialized = false;
			of_phy_put(session_shared_phy);
			session_shared_phy = NULL;
			return ret;
		}
		atc_core = apple_t8132_dp_get_regs(session_shared_phy);
		if (IS_ERR(atc_core)) {
			ret = PTR_ERR(atc_core);
			atc_core = NULL;
			return ret;
		}
		session_atc_owned = true;
		goto map_crossbar;
	}
	of_node_put(node);
	if (!request_mem_region(session_atc_base(), 0x24000, "m4-dcpext-atc"))
		return -EBUSY;
	session_atc_owned = true;
	atc_core = ioremap(session_atc_base(), 0x24000);
	if (!atc_core)
		return -ENOMEM;
map_crossbar:
	if (route_dpxbar) {
		/* ADT UFP endpoints 1/2, port 0 -> native selectors 2/4. */
		session_xbar.selector = 2 * (port + 1);
		if (!request_mem_region(session_atc_base() + 0x4c000, 0x4000, "m4-dcpext-dpxbar"))
			return -EBUSY;
		session_dpxbar_owned = true;
		session_dpxbar = ioremap(session_atc_base() + 0x4c000, 0x4000);
		if (!session_dpxbar)
			return -ENOMEM;
		return dpxbar_check_idle();
	}
	return 0;
}

static int session_atc_aux(bool enable)
{
	u32 power;
	int ret;
	if (!atc_core || !atc_pinned)
		return -ENODEV;
	ret = regmap_read(session_atc_pmgr, session_atc_power_offset(), &power);
	if (ret || power != session_atc_power)
		return ret ? ret : -EAGAIN;
	if (readl(atc_core + 0x20000) != 0x1b ||
	    !(readl(atc_core + 0x804) & 1))
		return -EIO;
	if (enable) {
		atc_aux_start();
		if ((readl(atc_core + ATC_AUX_CTRL) & 0x30f) != 0x10f ||
		    readl(atc_core + ATC_AUX_CFG) & 1)
			return -EIO;
	} else {
		/* Discovery can fail before firmware ever configures a main-link
		 * rate. A display release still requires the pre-discovery
		 * crossbar route to be parked before AUX is disabled. */
		if (session_dpxbar_selected) {
			ret = dpxbar_down();
			if (ret)
				return ret;
		}
		if (link_training && session_link.rate) {
			ret = dptx_link_set_rate(&session_link, 0);
			if (ret)
				return ret;
		}
		atc_aux_stop();
		if ((readl(atc_core + ATC_AUX_CTRL) & 0x20a) != 0x200 ||
		    !(readl(atc_core + ATC_AUX_CFG) & 1))
			return -EIO;
	}
	session_atc_active = enable;
	pr_info("m4_dcpext_session: ATC%u AUX callback enable=%u control=%#x config=%#x\n",
		session_atc_port, enable, readl(atc_core + ATC_AUX_CTRL), readl(atc_core + ATC_AUX_CFG));
	return 0;
}

static int session_atc_routed_protocol(void)
{
	int ret = dpxbar_up();
	if (ret)
		return ret;
	ret = session_atc_protocol();
	/* Firmware quiescence is required before clocks/routing can be removed.
	 * A failed protocol retains the complete active path for recovery.
	 */
	return ret ? ret : dpxbar_down();
}

/* Keep the active PHY lifetime independent of the long-running DCP loop:
 * an acknowledged display release can stop this PHY and select another
 * without restarting firmware or changing any DART/scanout allocation. */
static struct apple_t8132_atc_session session_atc_state;

static int session_atc_start_selected(bool route_pin_d)
{
	u32 power;
	int ret;

	ret = regmap_read(session_atc_pmgr, session_atc_power_offset(), &power);
	if (ret || power != session_atc_power)
		return ret ? ret : -EAGAIN;
	if (session_shared_phy) {
		if (!session_shared_power) {
			ret = phy_power_on(session_shared_phy);
			if (ret)
				return ret;
			session_shared_power = true;
		}
		if (!atc_pinned) {
			__module_get(THIS_MODULE);
			atc_pinned = true;
		}
		application_result = 0;
		return 0;
	}
	if (session_atc_state.touched)
		return -EBUSY;
	memset(&session_atc_state, 0, sizeof(session_atc_state));
	session_atc_state.regs = atc_core;
	session_atc_state.unflipped = !session_atc_flipped;
	memcpy(session_atc_state.tuning, atc_component_tunings,
	       sizeof(session_atc_state.tuning));
	ret = apple_t8132_atc_session_idle(&session_atc_state);
	if (ret)
		return ret;
	if (link_training)
		for (unsigned int i = 0; i < ARRAY_SIZE(atc_main_offsets); i++)
			atc_main_saved[i] = readl(atc_core + session_atc_main_offset(i));
	if (!atc_pinned) {
		__module_get(THIS_MODULE);
		atc_pinned = true;
	}
	ret = apple_t8132_atc_session_start(&session_atc_state, route_pin_d);
	application_result = ret;
	return ret;
}

static int session_atc_stop_selected(void)
{
	u32 power;
	int ret;

	if (session_dpxbar_selected || session_link.rate || session_atc_fault)
		return -EBUSY;
	if (session_shared_phy) {
		if (!session_shared_power)
			return 0;
		ret = phy_power_off(session_shared_phy);
		if (!ret)
			session_shared_power = false;
		return ret;
	}
	ret = apple_t8132_atc_session_stop(&session_atc_state);
	if (ret)
		return ret;
	if (link_training)
		for (unsigned int i = 0; i < ARRAY_SIZE(atc_main_offsets); i++) {
			u32 value = readl(atc_core + session_atc_main_offset(i));
			if (value != atc_main_saved[i]) {
				pr_err("m4_dcpext_session: main restoration failed offset=%#x got=%#x expected=%#x\n",
					session_atc_main_offset(i), value, atc_main_saved[i]);
				return -EIO;
			}
		}
	ret = regmap_read(session_atc_pmgr, session_atc_power_offset(), &power);
	return ret ? ret : power != session_atc_power ? -EAGAIN : 0;
}

/* Caller has received displayRelease ACK and the AUX-disable callback. */
static int session_atc_switch_port(unsigned int phy)
{
	int ret;

	if (phy > 1)
		return -EINVAL;
	if (session_atc_owned && phy == session_atc_port &&
	    session_atc_partner_flipped == session_atc_flipped)
		return session_shared_phy ? session_atc_start_selected(true) : 0;
	ret = session_atc_stop_selected();
	if (ret)
		return ret;
	session_atc_unmap();
#ifdef DCPEXT_DESKTOP
	/* find_partner already claimed the destination. On orientation-only
	 * changes it is also the old PHY: never drop that claim between unmap
	 * and prepare, or the peer controller can steal the live route. */
	if (phy != session_atc_port)
		m4_dcpext_route_release(port, session_atc_port);
#endif
	session_atc_port = phy;
	session_atc_active = false;
	memset(&session_link, 0, sizeof(session_link));
	ret = session_atc_prepare(true);
	if (ret)
		return ret;
	ret = session_atc_start_selected(true);
	if (!ret)
		pr_info("m4_dcpext_session: runtime route moved to ATC%u target=%#x\n",
			session_atc_port, session_atc_target());
	return ret;
}

static int session_atc_run(int (*observer)(void), bool route_pin_d)
{
	int ret = session_atc_start_selected(route_pin_d);

	if (ret)
		return ret;
	session_atc_protocol = observer;
	ret = session_dpxbar ? session_atc_routed_protocol() : observer();
	if (ret)
		return ret; /* Uncertain consumer still owns the complete route. */
	ret = session_atc_stop_selected();
	if (!ret && atc_pinned) {
		atc_pinned = false;
		session_atc_active = false;
		module_put(THIS_MODULE);
	}
	return ret;
}
