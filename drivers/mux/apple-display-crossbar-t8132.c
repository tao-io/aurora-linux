// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* T8132 ATC display crossbar. The J713 ATC1 / DCPEXT0 route is qualified.
 * Its register sequencing is shared with the bounded hardware qualification.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mux/driver.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "apple-display-crossbar-t8132.h"

struct t8132_xbar {
	struct apple_dpxbar_t8132 hw;
	struct mutex lock;
	bool failed;
};

static int t8132_xbar_set(struct mux_control *mux, int state)
{
	struct t8132_xbar *xbar = mux_chip_priv(mux->chip);
	int ret;

	/* The DT selector is (ufp_endpoint << 1) | ufp_port, not a DCP id.
	 * Other routes must be qualified independently before admitting them.
	 */
	if (state != 2 && state != MUX_IDLE_DISCONNECT)
		return -EOPNOTSUPP;
	guard(mutex)(&xbar->lock);
	if (xbar->failed)
		return -EIO;
	if (state == MUX_IDLE_DISCONNECT) {
		if (!xbar->hw.selected)
			return apple_dpxbar_t8132_idle(&xbar->hw);
		ret = apple_dpxbar_t8132_down(&xbar->hw);
		if (!ret)
			module_put(THIS_MODULE);
	} else {
		if (xbar->hw.selected)
			return -EBUSY;
		/* Keep the code, MMIO mapping and bound-device power reference
		 * alive if any write or status is uncertain. No user unbind ABI.
		 */
		if (!try_module_get(THIS_MODULE))
			return -ENODEV;
		ret = apple_dpxbar_t8132_up(&xbar->hw);
		if (ret && !xbar->hw.selected)
			module_put(THIS_MODULE);
	}
	if (ret && xbar->hw.selected)
		xbar->failed = true;
	return ret;
}

static const struct mux_control_ops t8132_xbar_ops = { .set = t8132_xbar_set };

static void t8132_xbar_power_put(void *data)
{
	pm_runtime_put_sync(data);
}

static int t8132_xbar_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct mux_chip *chip;
	struct t8132_xbar *xbar;
	int ret;

	if (!of_machine_is_compatible("apple,j713"))
		return -ENODEV;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x40b04c000ULL || resource_size(res) != 0x4000)
		return dev_err_probe(dev, -EINVAL, "unqualified crossbar resource\n");
	if (!dev->pm_domain)
		return dev_err_probe(dev, -EINVAL, "power domain is required\n");
	chip = devm_mux_chip_alloc(dev, 1, sizeof(*xbar));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	xbar = mux_chip_priv(chip);
	mutex_init(&xbar->lock);
	xbar->hw.regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(xbar->hw.regs))
		return PTR_ERR(xbar->hw.regs);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	/* One bound-device vote covers all mux transitions and failure retention.
	 * The PHY has its own vote; neither consumer borrows the other's count.
	 */
	ret = devm_add_action_or_reset(dev, t8132_xbar_power_put, dev);
	if (ret)
		return ret;
	chip->ops = &t8132_xbar_ops;
	chip->mux[0].states = 3;
	chip->mux[0].idle_state = MUX_IDLE_DISCONNECT;
	return devm_mux_chip_register(dev, chip);
}

static const struct of_device_id t8132_xbar_match[] = {
	{ .compatible = "apple,t8132-display-crossbar" },
	{ }
};
MODULE_DEVICE_TABLE(of, t8132_xbar_match);
static struct platform_driver t8132_xbar_driver = {
	.probe = t8132_xbar_probe,
	.driver = {
		.name = "apple-t8132-display-crossbar",
		.of_match_table = t8132_xbar_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(t8132_xbar_driver);
MODULE_DESCRIPTION("Apple T8132 ATC display crossbar");
MODULE_LICENSE("Dual MIT/GPL");
