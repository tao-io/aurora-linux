// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* T8132 shared high-speed PHY lifetime. USB and DP share reset, calibration,
 * lane routing and common power; neither consumer may reset the other's link.
 * All calibration is supplied by m1n1 from this machine's firmware ADT.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-apple-t8132.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/tunable.h>
#include "atc-t8132.h"
#include "atc-t8132-power.h"
#include "atc-t8132-common.h"

enum t8132_bank { CORE, AXI, LIOA, FABRIC, CONFIG, BANK_COUNT };
enum t8132_tuning { AXI_T, LIOA_T, FABRIC_T, CONFIG_T, COMMON_T, DP_T, EFUSE_T, USB0_T, USB1_T, TUNING_COUNT };
struct apple_t8132_atc {
	struct device *dev;
	struct mutex *lock;
	void __iomem *regs[BANK_COUNT];
	struct apple_tunable *tuning[TUNING_COUNT];
	unsigned int mode;
	bool route_pending;
	bool flipped, active, failed, usb, dp;
};
static const struct phy_ops t8132_shared_dp_ops;

static int t8132_stop(struct apple_t8132_atc *atc)
{
	int ret;

	lockdep_assert_held(atc->lock);
	if (atc->failed)
		return -EIO;
	if (atc->usb || atc->dp)
		return -EBUSY;
	if (!atc->active)
		return 0;
	if (atc->mode != 1)
		apple_t8132_atc_common_stop(atc->regs[CORE]);
	ret = apple_t8132_atc_power_down(atc->regs[CORE]);
	if (ret)
		atc->failed = true;
	else
		atc->active = false;
	return ret;
}

static int t8132_start(struct apple_t8132_atc *atc)
{
	void __iomem *core = atc->regs[CORE];
	int ret;

	lockdep_assert_held(atc->lock);
	if (atc->failed)
		return -EIO;
	if (atc->active)
		return 0;
	if (atc->mode != 1 && atc->mode != 4)
		return -ENOLINK;
	ret = apple_t8132_atc_power_idle(core);
	if (ret)
		return ret;
	ret = apple_t8132_atc_power_up_reset(core);
	if (ret)
		goto fail;
	apple_tunable_apply(atc->regs[AXI], atc->tuning[AXI_T]);
	apple_tunable_apply(atc->regs[LIOA], atc->tuning[LIOA_T]);
	apple_tunable_apply(atc->regs[FABRIC], atc->tuning[FABRIC_T]);
	apple_tunable_apply(atc->regs[CONFIG], atc->tuning[CONFIG_T]);
	apple_tunable_apply(core, atc->tuning[COMMON_T]);
	apple_tunable_apply(core, atc->tuning[atc->flipped ? USB1_T : USB0_T]);
	if (atc->mode != 1)
		apple_tunable_apply(core, atc->tuning[DP_T]);
	/* Native mode 1 and mode 4 use the same lane routing. Four-lane DP
	 * is deliberately not admitted until its routing is qualified. */
	ret = apple_t8132_atc_route_pin_d(core, atc->flipped);
	if (ret)
		goto fail;
	apple_tunable_apply(core, atc->tuning[EFUSE_T]);
	udelay(10);
	apple_t8132_atc_component_update(core, 0x20000, 0, 0x10);
	udelay(100);
	if (atc->mode != 1) {
		ret = apple_t8132_atc_common_start(core);
		if (ret)
			goto fail;
	}
	atc->active = true;
	dev_info(atc->dev, "shared PHY ready mode=%u flipped=%u\n", atc->mode, atc->flipped);
	return 0;
fail:
	atc->failed = true;
	dev_err(atc->dev, "high-speed PHY startup failed: %d\n", ret);
	return ret;
}

int apple_t8132_atc_configure(struct apple_t8132_atc *atc, unsigned int mode, bool flipped)
{
	int ret;

	lockdep_assert_held(atc->lock);
	if (mode != 0 && mode != 1 && mode != 4)
		return -EOPNOTSUPP;
	if (atc->mode == mode && atc->flipped == flipped) {
		atc->route_pending = false;
		return mode ? t8132_start(atc) : 0;
	}
	/* A display release is asynchronous. Keep the live route until its
	 * last consumer has retired; a new route must retry after release. */
	atc->route_pending = true;
	if (atc->usb || atc->dp)
		return -EBUSY;
	ret = t8132_stop(atc);
	if (ret)
		return ret;
	atc->mode = mode;
	atc->flipped = flipped;
	atc->route_pending = false;
	return mode ? t8132_start(atc) : 0;
}

int apple_t8132_atc_usb_get(struct apple_t8132_atc *atc)
{
	int ret;

	lockdep_assert_held(atc->lock);
	if (atc->route_pending)
		return -EAGAIN;
	if (atc->mode != 1 && atc->mode != 4)
		return -ENOLINK;
	ret = t8132_start(atc);
	if (!ret)
		atc->usb = true;
	return ret;
}

int apple_t8132_atc_usb_put(struct apple_t8132_atc *atc)
{
	lockdep_assert_held(atc->lock);
	atc->usb = false;
	return atc->dp ? 0 : t8132_stop(atc);
}

/* Do not allow genpd to remove power beneath a live or failed consumer.
 * Full system sleep requires both consumers to retire their links first. */
int apple_t8132_atc_suspend(struct apple_t8132_atc *atc)
{
	guard(mutex)(atc->lock);
	return atc->active || atc->failed || atc->usb || atc->dp ? -EBUSY : 0;
}

void __iomem *apple_t8132_atc_regs(struct apple_t8132_atc *atc)
{
	return atc->regs[CORE];
}

static int t8132_shared_dp_on(struct phy *phy)
{
	struct apple_t8132_atc *atc = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(atc->lock);
	if (atc->route_pending)
		return -EAGAIN;
	if (atc->mode != 4)
		return -ENOLINK;
	ret = t8132_start(atc);
	if (ret)
		return ret;
	apple_t8132_atc_aux_start(atc->regs[CORE]);
	atc->dp = true;
	return 0;
}

static int t8132_shared_dp_off(struct phy *phy)
{
	struct apple_t8132_atc *atc = phy_get_drvdata(phy);

	guard(mutex)(atc->lock);
	/* Main-link retirement belongs to the display consumer. */
	if (readl(atc->regs[CORE] + 0x7034) & 8)
		return -EBUSY;
	apple_t8132_atc_aux_stop(atc->regs[CORE]);
	atc->dp = false;
	return atc->usb ? 0 : t8132_stop(atc);
}

/* DP sees PD status before the debounced Type-C mux work necessarily runs.
 * A consumer must verify that its lane view matches the provider's route. */
int apple_t8132_dp_check_route(struct phy *phy, bool flipped)
{
	struct apple_t8132_atc *atc;

	if (!phy || phy->ops != &t8132_shared_dp_ops)
		return -EOPNOTSUPP;
	atc = phy_get_drvdata(phy);
	guard(mutex)(atc->lock);
	if (atc->failed)
		return -EIO;
	return atc->route_pending || atc->mode != 4 || atc->flipped != flipped ? -EAGAIN : 0;
}
EXPORT_SYMBOL_GPL(apple_t8132_dp_check_route);

/* Transitional register access for the existing DCP main-link implementation.
 * The PHY owns common power/reset; this lease only permits DP lane/AUX access.
 * The consumer must hold phy_power_on() until all firmware link work retires.
 */
void __iomem *apple_t8132_dp_get_regs(struct phy *phy)
{
	struct apple_t8132_atc *atc;

	if (!phy || phy->ops != &t8132_shared_dp_ops)
		return ERR_PTR(-EOPNOTSUPP);
	atc = phy_get_drvdata(phy);
	guard(mutex)(atc->lock);
	if (!atc->active || !atc->dp || atc->failed)
		return ERR_PTR(-EHOSTDOWN);
	return atc->regs[CORE];
}
EXPORT_SYMBOL_GPL(apple_t8132_dp_get_regs);

static const struct phy_ops t8132_shared_dp_ops = {
	.owner = THIS_MODULE,
	.power_on = t8132_shared_dp_on,
	.power_off = t8132_shared_dp_off,
};

struct apple_t8132_atc *apple_t8132_atc_create(struct platform_device *pdev,
					    struct mutex *lock, struct phy **dp)
{
	static const char * const banks[] = { "core", "axi2af", "lioa", "fabric", "common-config" };
	static const struct { const char *name; enum t8132_bank bank; } tunings[] = {
		{ "apple,tunable-usb3-axi2af", AXI },
		{ "apple,tunable-usb3-lioa", LIOA },
		{ "apple,tunable-usb3-fabric", FABRIC },
		{ "apple,tunable-usb3-config", CONFIG },
		{ "apple,tunable-usb3-common", CORE },
		{ "apple,tunable-usb3-dp", CORE },
		{ "apple,tunable-usb3-efuse", CORE },
		{ "apple,tunable-lane0-usb", CORE },
		{ "apple,tunable-lane1-usb", CORE },
	};
	struct device *dev = &pdev->dev;
	struct resource *res[BANK_COUNT];
	struct apple_t8132_atc *atc;

	/* Older handoffs keep the existing USB2-only driver. */
	if (!platform_get_resource_byname(pdev, IORESOURCE_MEM, "common-config"))
		return NULL;
	atc = devm_kzalloc(dev, sizeof(*atc), GFP_KERNEL);
	if (!atc)
		return ERR_PTR(-ENOMEM);
	atc->dev = dev;
	atc->lock = lock;
	for (unsigned int i = 0; i < BANK_COUNT; i++) {
		res[i] = platform_get_resource_byname(pdev, IORESOURCE_MEM, banks[i]);
		atc->regs[i] = devm_ioremap_resource(dev, res[i]);
		if (IS_ERR(atc->regs[i]))
			return ERR_CAST(atc->regs[i]);
	}
	for (unsigned int i = 0; i < TUNING_COUNT; i++) {
		atc->tuning[i] = devm_apple_tunable_parse(dev, dev->of_node,
						       tunings[i].name, res[tunings[i].bank]);
		if (IS_ERR(atc->tuning[i]))
			return ERR_CAST(atc->tuning[i]);
	}
	*dp = devm_phy_create(dev, NULL, &t8132_shared_dp_ops);
	if (IS_ERR(*dp))
		return ERR_CAST(*dp);
	phy_set_drvdata(*dp, atc);
	return atc;
}
