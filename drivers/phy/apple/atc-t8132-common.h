/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Independent implementation of T8132 common/AUX register operations observed
 * in AppleT8132TypeCPhy native routines. Exact image/function manifests and
 * synthetic executions are retained under build/displayport-kernelcache.
 * Tunable transport follows Asahi m1n1 / Linux ATC; calibration values come
 * from the live device tree, never another board's hardcoded values.
 * This is a partial component experiment, not full PHY/USB/DP initialization.
 */
#ifndef APPLE_ATCPHY_T8132_COMMON_H
#define APPLE_ATCPHY_T8132_COMMON_H
struct apple_t8132_tuning { u32 offset, mask, value; };
#define APPLE_T8132_COMMON_TUNINGS \
{ 0x800, 0x3f0000, 0 }, { 0x2224, 0xc00, 0 }, \
{ 0x2230, 0x1f, 0 }, { 0x2240, 0x18007f, 0 }, \
{ 0x22b0, 0x8700000, 0 }, { 0x22e8, 0x60001, 0 }, \
{ 0x2338, 0xf800000, 0 }, { 0x16008, 0x38, 0 }, \
{ 0x1600c, 0x1f, 0 }, { 0xa04, 0x7c, 0 }, \
{ 0x260c, 0x3ff, 0 },

static const struct apple_t8132_tuning apple_t8132_tuning_specs[] = { APPLE_T8132_COMMON_TUNINGS };


static int apple_t8132_atc_component_select(struct apple_t8132_tuning *selected, const u32 *cells, unsigned int count)
{
	bool found[ARRAY_SIZE(apple_t8132_tuning_specs)] = { false };
	u32 values[ARRAY_SIZE(apple_t8132_tuning_specs)];
	if (!count || count > 384 || count % 3)
		return -EINVAL;
	for (unsigned int i = 0; i < count; i += 3)
		for (unsigned int j = 0; j < ARRAY_SIZE(apple_t8132_tuning_specs); j++) {
			const struct apple_t8132_tuning *t = &apple_t8132_tuning_specs[j];
			if (cells[i] != t->offset)
				continue;
			if (found[j] || cells[i + 1] != t->mask || cells[i + 2] & ~t->mask)
				return -EINVAL;
			found[j] = true;
			values[j] = cells[i + 2];
		}
	for (unsigned int j = 0; j < ARRAY_SIZE(apple_t8132_tuning_specs); j++)
		if (!found[j])
			return -ENOENT;
	for (unsigned int j = 0; j < ARRAY_SIZE(apple_t8132_tuning_specs); j++) {
		selected[j] = apple_t8132_tuning_specs[j];
		selected[j].value = values[j];
	}
	return 0;
}

/* Native common pairs have one delay after each pair of separate writes. */
static void apple_t8132_atc_component_update(void __iomem *regs, u32 offset, u32 clear, u32 set)
{
	writel((readl(regs + offset) & ~clear) | set, regs + offset);
}

/* Native T8132 aciophy_xbar_config(mode=4, flipped={0,1}), then the flip
 * write in aciophy_phy_init_partC before its 10-us reset-release delay.
 * Asahi's atcphy_configure_lanes provides the lane/protocol interpretation.
 * This selects two DP lanes; it does not initialize or start USB3 lanes.
 */
static int apple_t8132_atc_route_pin_d(void __iomem *regs, bool flipped)
{
	/* Retained native xbar-mode4-flip{0,1} traces: normal uses pair1,
	 * flipped uses pair0. MISC bit2 is written before reset release. */
	u32 lanes = flipped ? 0x252 : 0x489;
	u32 crossbar = flipped ? 0x111 : 0x110;

	apple_t8132_atc_component_update(regs, 0x60, 0xfff, lanes);
	apple_t8132_atc_component_update(regs, 0x64, 0x1ffff, crossbar);
	apple_t8132_atc_component_update(regs, 0x20008, 4, flipped ? 4 : 0);
	pr_info("m4_atc_probe: pin-D flipped=%u routing lanes=%#x crossbar=%#x misc=%#x\n",
		flipped, readl(regs + 0x60), readl(regs + 0x64), readl(regs + 0x20008));
	return (readl(regs + 0x60) & 0xfff) == lanes &&
	       (readl(regs + 0x64) & 0x1ffff) == crossbar &&
	       !!(readl(regs + 0x20008) & 4) == flipped ? 0 : -EIO;
}

static inline int apple_t8132_atc_route_pin_d_flipped(void __iomem *regs)
{
	return apple_t8132_atc_route_pin_d(regs, true);
}

static int apple_t8132_atc_common_start(void __iomem *regs)
{
	u32 status;
	apple_t8132_atc_component_update(regs, 8, 0, 4);
	apple_t8132_atc_component_update(regs, 8, 0, 8);
	udelay(1);
	apple_t8132_atc_component_update(regs, 8, 0, 1);
	apple_t8132_atc_component_update(regs, 8, 0, 2);
	udelay(1);
	apple_t8132_atc_component_update(regs, 8, 0x10, 0);
	apple_t8132_atc_component_update(regs, 8, 0, 0x20);
	udelay(1);
	apple_t8132_atc_component_update(regs, 0xa00, 0, 2);
	udelay(15);
	return readl_poll_timeout(regs + 0x804, status, status & 1, 1, 100);
}

static void apple_t8132_atc_common_stop(void __iomem *regs)
{
	apple_t8132_atc_component_update(regs, 0xa00, 2, 0);
	udelay(1);
	apple_t8132_atc_component_update(regs, 8, 0, 0x10);
	udelay(1);
	apple_t8132_atc_component_update(regs, 8, 1, 0);
	udelay(1);
	apple_t8132_atc_component_update(regs, 8, 4, 0);
	udelay(1);
}

#define ATC_AUX_CTRL 0x16400
#define ATC_AUX_CFG 0x16000

static void apple_t8132_atc_aux_start(void __iomem *regs)
{
	apple_t8132_atc_component_update(regs, ATC_AUX_CTRL, 0, 3);
	udelay(1);
	apple_t8132_atc_component_update(regs, ATC_AUX_CTRL, 0, 0xc);
	udelay(1);
	apple_t8132_atc_component_update(regs, ATC_AUX_CTRL, 0x300, 0x100);
	udelay(1);
	apple_t8132_atc_component_update(regs, ATC_AUX_CFG, 1, 0);
	udelay(1);
}

static void apple_t8132_atc_aux_stop(void __iomem *regs)
{
	apple_t8132_atc_component_update(regs, ATC_AUX_CFG, 0, 1);
	udelay(1);
	apple_t8132_atc_component_update(regs, ATC_AUX_CTRL, 0, 0x200);
	udelay(1);
	apple_t8132_atc_component_update(regs, ATC_AUX_CTRL, 8, 0);
	udelay(1);
	apple_t8132_atc_component_update(regs, ATC_AUX_CTRL, 2, 0);
	udelay(1);
}


#endif
