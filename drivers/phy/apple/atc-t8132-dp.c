// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J713 ATC1 DP provider. USB2 remains owned by atc.c; no USB3 enablement.
 * Register operations are shared with the guarded ATC qualification module.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-apple-t8132.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/usb/typec_dp.h>
#include "atc-t8132-dp.h"
#include "atc-t8132-session.h"
#include "atc-t8132-dp-main.h"
#include "atc-t8132-dp-drive.h"
#include "atc-t8132-registers.h"

struct apple_t8132_dp {
	struct apple_t8132_atc_session session;
	struct mutex *lock;
	struct apple_t8132_dp_connection connection;
	apple_t8132_dp_notify_t notify;
	void *notify_cookie;
	bool pinned;
	bool aux;
	u32 rate;
	u32 lanes;
	u32 saved[ARRAY_SIZE(apple_t8132_atc_main_offsets)];
};

static const struct phy_ops t8132_dp_ops;

static void t8132_dp_notify(struct apple_t8132_dp *dp)
{
	struct apple_t8132_dp_connection snapshot = dp->connection;

	lockdep_assert_held(dp->lock);
	if (dp->notify)
		dp->notify(dp->notify_cookie, &snapshot);
}

void apple_t8132_dp_orientation(struct apple_t8132_dp *dp,
			       enum typec_orientation orientation)
{
	lockdep_assert_held(dp->lock);
	if (dp->connection.orientation == orientation)
		return;
	dp->connection.orientation = orientation;
	dp->connection.pin_d = false;
	dp->connection.allowed = false;
	dp->connection.hpd = false;
	dp->connection.generation++;
	t8132_dp_notify(dp);
}

int apple_t8132_dp_route(struct apple_t8132_dp *dp, bool pin_d,
			const struct typec_displayport_data *data)
{
	struct apple_t8132_dp_connection *c = &dp->connection;
	bool allowed, hpd, irq;
	int ret = 0;

	lockdep_assert_held(dp->lock);
	/* Missing DP status must revoke a previous route, not leave stale HPD. */
	if (pin_d && !data) {
		pin_d = false;
		ret = -EINVAL;
	}
	allowed = pin_d && c->orientation == TYPEC_ORIENTATION_REVERSE;
	hpd = allowed && (data->status & DP_STATUS_HPD_STATE);
	irq = hpd && (data->status & DP_STATUS_IRQ_HPD);
	if (c->pin_d != pin_d || c->allowed != allowed || c->hpd != hpd || irq) {
		c->pin_d = pin_d;
		c->allowed = allowed;
		c->hpd = hpd;
		c->generation++;
		if (irq)
			c->irq_count++;
		t8132_dp_notify(dp);
	}
	return ret ?: (pin_d && !allowed ? -EOPNOTSUPP : 0);
}

int apple_t8132_dp_subscribe(struct phy *phy, apple_t8132_dp_notify_t notify,
			    void *cookie)
{
	struct apple_t8132_dp *dp;

	if (!phy || phy->ops != &t8132_dp_ops)
		return -EOPNOTSUPP;
	if (!notify)
		return -EINVAL;
	dp = phy_get_drvdata(phy);
	guard(mutex)(dp->lock);
	if (dp->notify)
		return -EBUSY;
	dp->notify = notify;
	dp->notify_cookie = cookie;
	t8132_dp_notify(dp);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_t8132_dp_subscribe);

int apple_t8132_dp_unsubscribe(struct phy *phy, apple_t8132_dp_notify_t notify,
			      void *cookie)
{
	struct apple_t8132_dp *dp;

	if (!phy || phy->ops != &t8132_dp_ops)
		return -EOPNOTSUPP;
	if (!notify)
		return -EINVAL;
	dp = phy_get_drvdata(phy);
	guard(mutex)(dp->lock);
	if (dp->notify != notify || dp->notify_cookie != cookie)
		return -ENOENT;
	dp->notify = NULL;
	dp->notify_cookie = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_t8132_dp_unsubscribe);

int apple_t8132_dp_suspend(struct apple_t8132_dp *dp)
{
	guard(mutex)(dp->lock);
	/* Do not let genpd remove clocks underneath a live or failed consumer.
	 * The DCP consumer must retire its route before the provider suspends.
	 */
	return dp->session.touched || dp->pinned ? -EBUSY : 0;
}

static int t8132_dp_power_on(struct phy *phy)
{
	struct apple_t8132_dp *dp = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(dp->lock);
	if (dp->session.failed)
		return -EIO;
	if (!dp->connection.allowed)
		return -ENOLINK;
	if (dp->session.active)
		return 0;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	dp->pinned = true;
	for (unsigned int i = 0; i < ARRAY_SIZE(dp->saved); i++)
		dp->saved[i] = readl(dp->session.regs + apple_t8132_atc_main_offsets[i]);
	ret = apple_t8132_atc_session_start(&dp->session, true);
	if (!ret)
		dp->aux = true;
	if (ret && !dp->session.touched) {
		dp->pinned = false;
		module_put(THIS_MODULE);
	}
	return ret;
}

static int t8132_dp_stop_link(struct apple_t8132_dp *dp)
{
	int ret;

	if (!dp->rate)
		return 0;
	ret = apple_t8132_atc_dp_main_stop(dp->session.regs, 1);
	if (ret)
		dp->session.failed = true;
	else
		dp->rate = 0;
	return ret;
}

static int t8132_dp_power_off(struct phy *phy)
{
	struct apple_t8132_dp *dp = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(dp->lock);
	if (dp->session.failed)
		return -EIO;
	ret = t8132_dp_stop_link(dp);
	if (!ret)
		ret = apple_t8132_atc_session_stop(&dp->session);
	if (!ret && dp->pinned)
		for (unsigned int i = 0; i < ARRAY_SIZE(dp->saved); i++) {
			if (readl(dp->session.regs + apple_t8132_atc_main_offsets[i]) !=
			    dp->saved[i]) {
				/* Power reset alone is insufficient evidence of restoration. */
				dp->session.touched = dp->session.failed = true;
				ret = -EIO;
				break;
			}
		}
	if (!ret) {
		dp->aux = false;
		dp->lanes = 0;
		if (dp->pinned) {
			dp->pinned = false;
			module_put(THIS_MODULE);
		}
	}
	return ret;
}

static int t8132_dp_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct apple_t8132_dp *dp = phy_get_drvdata(phy);
	bool enable = mode == PHY_MODE_DP;
	u32 control, config;

	if (submode || (mode != PHY_MODE_DP && mode != PHY_MODE_INVALID))
		return -EINVAL;
	guard(mutex)(dp->lock);
	if (dp->session.failed)
		return -EIO;
	if (!dp->session.active)
		return -EHOSTDOWN;
	if (enable && !dp->connection.allowed)
		return -ENOLINK;
	if (!enable && dp->rate)
		return -EBUSY;
	/* DPTX activate/deactivate affects AUX only. Keep common power and its
	 * ownership reference until the display consumer calls power_off().
	 */
	if (enable)
		apple_t8132_atc_aux_start(dp->session.regs);
	else
		apple_t8132_atc_aux_stop(dp->session.regs);
	control = readl(dp->session.regs + ATC_AUX_CTRL);
	config = readl(dp->session.regs + ATC_AUX_CFG);
	if (enable ? (control & 0x30f) != 0x10f || (config & 1) :
		     (control & 0x20a) != 0x200 || !(config & 1)) {
		dp->session.failed = true;
		return -EIO;
	}
	dp->aux = enable;
	return 0;
}

static int t8132_dp_validate(struct phy *phy, enum phy_mode mode, int submode,
			      union phy_configure_opts *opts)
{
	struct apple_t8132_dp *dp = phy_get_drvdata(phy);

	if (mode != PHY_MODE_DP || submode)
		return -EINVAL;
	guard(mutex)(dp->lock);
	opts->dp.lanes = dp->connection.allowed && !dp->session.failed ? 2 : 0;
	return 0;
}

static int t8132_dp_configure(struct phy *phy, union phy_configure_opts *opts)
{
	/* Native fallback training table, captured for J713 without an ADT
	 * dp-training-table override. DP lane0 uses RX, lane1 uses TX of pair0.
	 */
	static const u32 presets[16] = {
		0x3f024, 0x237013, 0x37200b, 0x46e004,
		0x3f01c, 0x23700b, 0x372004, 0x3f024,
		0x3f013, 0x237004, 0x3f024, 0x3f024,
		0x3f004, 0x3f024, 0x3f024, 0x3f024,
	};
	struct apple_t8132_dp *dp = phy_get_drvdata(phy);
	struct phy_configure_opts_dp *o = &opts->dp;
	u32 lanes;
	int ret = 0;

	guard(mutex)(dp->lock);
	if (dp->session.failed)
		return -EIO;
	if (!dp->session.active)
		return -EHOSTDOWN;
	if (!dp->aux && ((o->set_rate && o->link_rate) || o->set_voltages))
		return -EHOSTDOWN;
	/* Teardown must remain possible after Type-C invalidates the route. */
	if (!dp->connection.allowed && !(o->set_rate && !o->link_rate &&
			      !o->set_voltages && (!o->set_lanes || !o->lanes)))
		return -ENOLINK;
	lanes = o->set_lanes ? o->lanes : dp->lanes;
	if (o->ssc || (o->set_lanes && lanes != 0 && lanes != 2) ||
	    (o->set_rate && o->link_rate != 0 && o->link_rate != 2700))
		return -EOPNOTSUPP;
	if ((o->set_rate && o->link_rate && lanes != 2) ||
	    (o->set_voltages && (lanes != 2 || !(o->set_rate ? o->link_rate : dp->rate))))
		return -EINVAL;
	/* Validate the whole request before any register or cached-state change. */
	if (o->set_voltages)
		for (unsigned int i = 0; i < 2; i++)
			if (o->voltage[i] > 3 || o->pre[i] > 3 || o->voltage[i] + o->pre[i] > 3)
				return -EINVAL;
	if (o->set_rate && o->link_rate != dp->rate) {
		ret = t8132_dp_stop_link(dp);
		if (!ret && o->link_rate)
			ret = apple_t8132_atc_dp_main_prepare(dp->session.regs, 1, 10, false);
		if (ret)
			goto fail;
		dp->rate = o->link_rate;
	}
	if (o->set_lanes)
		dp->lanes = lanes;
	if (o->set_voltages)
		for (unsigned int i = 0; i < 2; i++) {
			ret = apple_t8132_atc_dp_drive_preset(dp->session.regs, 0, i == 1,
					presets[o->voltage[i] * 4 + o->pre[i]]);
			if (ret)
				goto fail;
		}
	return 0;
fail:
	dp->session.failed = true;
	return ret;
}

static const struct phy_ops t8132_dp_ops = {
	.owner = THIS_MODULE,
	.power_on = t8132_dp_power_on,
	.power_off = t8132_dp_power_off,
	.set_mode = t8132_dp_set_mode,
	.validate = t8132_dp_validate,
	.configure = t8132_dp_configure,
};

static void t8132_dp_pm_put(void *data)
{
	pm_runtime_put_sync(data);
}

struct apple_t8132_dp *apple_t8132_dp_create(struct platform_device *pdev,
					  struct mutex *lock, struct phy **phy)
{
	struct device *dev = &pdev->dev;
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "core");
	struct apple_t8132_dp *dp;
	u32 *cells;
	int count, ret;

	/* Old reservations-only DTs keep the existing USB2 provider usable. */
	if (!res || !dev->pm_domain)
		return NULL;
	if (!of_machine_is_compatible("apple,j713") ||
	    res->start != 0x40b000000ULL || resource_size(res) != 0x24000)
		return ERR_PTR(-EOPNOTSUPP);
	/* A different board calibration table needs separate qualification. */
	if (of_find_property(dev->of_node, "apple,dp-training-table", NULL) ||
	    of_find_property(dev->of_node, "apple,dp-training-table-hbr", NULL))
		return ERR_PTR(-EOPNOTSUPP);
	count = of_property_count_u32_elems(dev->of_node, "apple,tunable-common-b");
	if (count <= 0 || count > 384 || count % 3)
		return ERR_PTR(-EINVAL);
	dp = devm_kzalloc(dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return ERR_PTR(-ENOMEM);
	cells = kmalloc_array(count, sizeof(*cells), GFP_KERNEL);
	if (!cells)
		return ERR_PTR(-ENOMEM);
	ret = of_property_read_u32_array(dev->of_node, "apple,tunable-common-b", cells, count);
	if (!ret)
		ret = apple_t8132_atc_component_select(dp->session.tuning, cells, count);
	kfree(cells);
	if (ret)
		return ERR_PTR(ret);
	dp->session.regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(dp->session.regs))
		return ERR_CAST(dp->session.regs);
	dp->lock = lock;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ERR_PTR(ret);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ERR_PTR(ret);
	ret = devm_add_action_or_reset(dev, t8132_dp_pm_put, dev);
	if (ret)
		return ERR_PTR(ret);
	*phy = devm_phy_create(dev, NULL, &t8132_dp_ops);
	if (IS_ERR(*phy))
		return ERR_CAST(*phy);
	phy_set_drvdata(*phy, dp);
	return dp;
}
