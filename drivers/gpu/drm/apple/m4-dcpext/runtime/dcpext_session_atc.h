/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* ATC1 ownership for the combined external-DCP/partial-AUX experiment.
 * Reuses the independently hardware-qualified ATC component implementation.
 * HPD trials additionally select the admitted flipped pin-D lane routing.
 * No USB2, AXI lock or clock-provider changes are performed.
 */
#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <linux/spmi.h>
#include <linux/workqueue.h>
#include "tps6598x.h"
static void __iomem *atc_core;
static bool atc_pinned, session_atc_owned, session_atc_active, session_atc_fault;
static bool session_atc_hpd;
static struct regmap *session_atc_pmgr;
static u32 session_atc_power;
#include "dcpext_atc_power.h"
#include "dcpext_atc_common.h"
#include "dcpext_dpxbar.h"
#include "dcpext_atc_dp_main.h"
#include "dcpext_atc_dp_drive.h"
#include "dcpext_atc_main_snapshot.h"
#include "dcpext_dptx_link.h"
static bool link_training;
module_param(link_training, bool, 0400);
MODULE_PARM_DESC(link_training, "Guarded HBR/two-lane/native-fallback-drive training trial; no scanout");
static struct dptx_link_state session_link;
static int (*session_atc_protocol)(void);

static int session_atc_check_partner(void)
{
	/* Cached TIPD state only, following m4_typec_probe. Private layout and
	 * definitions: Intel/Heikki Krogerus and Asahi, tps6598x.h at fba91e5d9577.
	 * Current scope is the captured flipped pin-D adapter on SPMI 1-0a.
	 */
	struct device_node *node;
	struct spmi_device *sdev;
	struct tps6598x *tps;
	u32 status = 0, data = 0;
	int ret = -ENODEV;
	for_each_compatible_node(node, NULL, "apple,t8132-sn201202x") {
		sdev = spmi_find_device_by_of_node(node);
		if (!sdev)
			continue;
		if (strcmp(dev_name(&sdev->dev), "1-0a")) {
			spmi_device_put(sdev);
			continue;
		}
		device_lock(&sdev->dev);
		tps = spmi_device_get_drvdata(sdev);
		if (sdev->dev.driver && tps && tps->dev == &sdev->dev) {
			mutex_lock(&tps->lock);
			status = tps->status;
			data = tps->data_status;
			session_atc_hpd = !!(data & CD321X_DATA_STATUS_HPD_LEVEL);
			mutex_unlock(&tps->lock);
			ret = status & TPS_STATUS_PLUG_PRESENT &&
			      status & TPS_STATUS_PLUG_UPSIDE_DOWN &&
			      data & TPS_DATA_STATUS_DP_CONNECTION &&
			      TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT(data) == TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_D ? 0 : -ENOLINK;
		}
		device_unlock(&sdev->dev);
		spmi_device_put(sdev);
		of_node_put(node);
		pr_info("m4_dcpext_session: ATC1 cached Type-C status=%#x data=%#x pin-D/flipped admission=%d\n",
			status, data, ret);
		return ret;
	}
	return ret;
}

static int dptx_link_set_rate(struct dptx_link_state *s, u32 rate)
{
	int ret;
	if (s->fault || session_atc_fault)
		return -EIO;
	if (rate != 0 && rate != 0xa)
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
		if ((readl(atc_core + 0x60) & 0xfff) != 0x252 ||
		    (readl(atc_core + 0x64) & 0x1ffff) != 0x111)
			return -EIO;
		ret = atc_dp_main_prepare(1, rate, false);
	} else {
		ret = atc_dp_main_stop(1);
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
	if (!atc_pinned || !session_atc_active || s->rate != 0xa ||
	    s->lanes != 2 || !(readl(atc_core + 0x7034) & 8))
		return -EIO;
	ret = session_atc_check_partner();
	if (ret)
		return ret;
	/* Native pair0 order: DP lane0 RX transmitter, DP lane1 TX transmitter. */
	rx = table[drive[1] * 4 + drive[2]];
	tx = table[drive[4] * 4 + drive[5]];
	ret = atc_dp_drive_preset(0, false, rx);
	if (!ret)
		ret = atc_dp_drive_preset(0, true, tx);
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

static void session_atc_cleanup(void)
{
	if (session_dpxbar)
		iounmap(session_dpxbar);
	session_dpxbar = NULL;
	if (session_dpxbar_owned)
		release_mem_region(0x40b04c000ULL, 0x4000);
	session_dpxbar_owned = false;
	if (atc_core)
		iounmap(atc_core);
	atc_core = NULL;
	if (session_atc_owned)
		release_mem_region(0x40b000000ULL, 0x24000);
	session_atc_owned = false;
}

static int session_atc_prepare(bool route_dpxbar)
{
	struct device_node *node;
	u32 cells[384];
	int count, ret;
	ret = session_atc_check_partner();
	if (ret)
		return ret;
	node = of_find_node_by_path("/soc/phy@40aa90000");
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
	ret = regmap_read(session_atc_pmgr, 0x428, &session_atc_power);
	if (ret)
		return ret;
	if ((session_atc_power & 0xff) != 0xff ||
	    session_atc_power & (BIT(31) | BIT(28) | BIT(12) | BIT(11) | BIT(10)))
		return -EHOSTDOWN;
	if (!request_mem_region(0x40b000000ULL, 0x24000, "m4-dcpext-atc1"))
		return -EBUSY;
	session_atc_owned = true;
	atc_core = ioremap(0x40b000000ULL, 0x24000);
	if (!atc_core)
		return -ENOMEM;
	if (route_dpxbar) {
		if (!request_mem_region(0x40b04c000ULL, 0x4000, "m4-dcpext-dpxbar1"))
			return -EBUSY;
		session_dpxbar_owned = true;
		session_dpxbar = ioremap(0x40b04c000ULL, 0x4000);
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
	ret = regmap_read(session_atc_pmgr, 0x428, &power);
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
	pr_info("m4_dcpext_session: ATC1 AUX callback enable=%u control=%#x config=%#x\n",
		enable, readl(atc_core + ATC_AUX_CTRL), readl(atc_core + ATC_AUX_CFG));
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

static int session_atc_run(int (*observer)(void), bool route_pin_d)
{
	u32 power;
	int ret;
	/* The domain was admitted before private DART publication; recheck it
	 * immediately before touching the PHY, after the mapping was installed.
	 */
	ret = regmap_read(session_atc_pmgr, 0x428, &power);
	if (ret || power != session_atc_power)
		return ret ? ret : -EAGAIN;
	session_atc_protocol = observer;
	atc_component_observer = session_dpxbar ? session_atc_routed_protocol : observer;
	atc_component_route_pin_d = route_pin_d;
	if (link_training)
		for (unsigned int i = 0; i < ARRAY_SIZE(atc_main_offsets); i++)
			atc_main_saved[i] = readl(atc_core + atc_main_offsets[i]);
	ret = atc_common_roundtrip();
	if (!ret && link_training)
		for (unsigned int i = 0; i < ARRAY_SIZE(atc_main_offsets); i++) {
			u32 value = readl(atc_core + atc_main_offsets[i]);
			if (value != atc_main_saved[i]) {
				pr_err("m4_dcpext_session: main restoration failed offset=%#x got=%#x expected=%#x\n",
					atc_main_offsets[i], value, atc_main_saved[i]);
				ret = -EIO;
				break;
			}
		}
	if (!ret) {
		ret = regmap_read(session_atc_pmgr, 0x428, &power);
		if (!ret && power != session_atc_power)
			ret = -EAGAIN;
	}
	if (!ret && atc_pinned) {
		atc_pinned = false;
		session_atc_active = false;
		module_put(THIS_MODULE);
	}
	return ret;
}
