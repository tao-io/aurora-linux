/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* J713 ATC1 DPXBAR component. Register roles and implementation reference:
 * Sven Peter / Asahi apple-display-crossbar.c. Exact transition masks/order:
 * independently observed in the saved native AppleT602XATCDPXBAR routines.
 * Native IODPTXPort consumes dp-switch-ufp-endpoint=1 and port=0 directly;
 * getMuxSelector computes (endpoint << 1) | (port & 1), hence selector 2.
 * Only an exclusively owned, clock-stopped snapshot is admitted. Native
 * takeConnectionDown retains bit8 at0x34; hardware ignores clearing it.
 * This one exact persistent setting is admitted alongside its reset value.
 * All remaining control/status samples must match the captured idle state.
 */
#ifndef APPLE_DISPLAY_CROSSBAR_T8132_H
#define APPLE_DISPLAY_CROSSBAR_T8132_H
/* Caller holds its device lock and a runtime PM reference throughout each
 * operation. selected records an attempted route until a verified idle state;
 * it is deliberately retained after uncertain writes or failed shutdown. */
struct apple_dpxbar_t8132 {
	void __iomem *regs;
	bool selected;
};
static const u32 dpxbar_offsets[] = { 0, 4, 8, 0xc, 0x10, 0x14, 0x18,
	0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34,
	0x800, 0x804, 0x808, 0x80c, 0x810, 0x814, 0x818, 0x81c };
static const u32 dpxbar_idle[] = { 0, 0x1ff, 0, 0, 0, 0x1ff, 0,
	0, 0, 0x111, 0, 0, 0, 0, 0, 0x1ff, 0, 0, 0x1ff, 0, 0, 0x111 };

static int apple_dpxbar_t8132_idle(struct apple_dpxbar_t8132 *xbar)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(dpxbar_offsets); i++) {
		u32 value = readl(xbar->regs + dpxbar_offsets[i]);
		if (dpxbar_offsets[i] == 0x34 && value == 0x100)
			continue;
		if (value != dpxbar_idle[i]) {
			pr_err("apple-t8132-dpxbar: idle mismatch offset=%#x value=%#x expected=%#x\n",
				dpxbar_offsets[i], value, dpxbar_idle[i]);
			return -EBUSY;
		}
	}
	return 0;
}

static void apple_dpxbar_t8132_update(struct apple_dpxbar_t8132 *xbar, u32 offset, u32 clear, u32 set)
{
	writel((readl(xbar->regs + offset) & ~clear) | set,
	       xbar->regs + offset);
}

static int apple_dpxbar_t8132_up(struct apple_dpxbar_t8132 *xbar)
{
	int ret = apple_dpxbar_t8132_idle(xbar);
	if (ret)
		return ret;
	/* Caller already pins the powered PHY and published DART tables. */
	xbar->selected = true;
	apple_dpxbar_t8132_update(xbar, 0x30, 0xf00000, 2 << 20);
	apple_dpxbar_t8132_update(xbar, 0x30, 0xf00, 2 << 8);
	if (readl(xbar->regs + 0x30) != 0x200200)
		return -EIO;
	/* Native connection-up uses source bit 4 and physical-output bit 0x100.
	 * Clock class 1 follows native pclkForConnection for attributes 0x100.
	 */
	apple_dpxbar_t8132_update(xbar, 4, 4, 0);
	apple_dpxbar_t8132_update(xbar, 0x14, 4, 0);
	apple_dpxbar_t8132_update(xbar, 0x24, 0x100, 0);
	udelay(1);
	if (readl(xbar->regs + 0x804) & 4 ||
	    readl(xbar->regs + 0x810) & 4 ||
	    readl(xbar->regs + 0x81c) & 0x100)
		return -ETIMEDOUT;
	apple_dpxbar_t8132_update(xbar, 8, 0, 4);
	apple_dpxbar_t8132_update(xbar, 0x18, 0x30, 0x10);
	apple_dpxbar_t8132_update(xbar, 0x28, 0x300, 0x100);
	apple_dpxbar_t8132_update(xbar, 0, 0, 4);
	apple_dpxbar_t8132_update(xbar, 0xc, 0, 4);
	apple_dpxbar_t8132_update(xbar, 0x1c, 0, 0x100);
	apple_dpxbar_t8132_update(xbar, 0x34, 0, 0x100);
	apple_dpxbar_t8132_update(xbar, 0x2c, 0, 4);
	pr_info("apple-t8132-dpxbar: selector2 up mux=%#x source_clock=%#x output_clock=%#x\n",
		readl(xbar->regs + 0x30), readl(xbar->regs),
		readl(xbar->regs + 0x1c));
	if (readl(xbar->regs) != 4 || readl(xbar->regs + 4) != 0x1fb ||
	    readl(xbar->regs + 8) != 4 || readl(xbar->regs + 0xc) != 4 ||
	    readl(xbar->regs + 0x14) != 0x1fb ||
	    readl(xbar->regs + 0x18) != 0x10 ||
	    readl(xbar->regs + 0x1c) != 0x100 ||
	    readl(xbar->regs + 0x24) != 0x11 ||
	    readl(xbar->regs + 0x28) != 0x100 ||
	    readl(xbar->regs + 0x2c) != 4 ||
	    readl(xbar->regs + 0x34) != 0x100)
		return -EIO;
	return 0;
}

static int apple_dpxbar_t8132_down(struct apple_dpxbar_t8132 *xbar)
{
	u32 source, read_clock, output;
	if (!xbar->selected)
		return 0;
	apple_dpxbar_t8132_update(xbar, 0x2c, 4, 0);
	apple_dpxbar_t8132_update(xbar, 0, 4, 0);
	apple_dpxbar_t8132_update(xbar, 0xc, 4, 0);
	apple_dpxbar_t8132_update(xbar, 0x1c, 0x100, 0);
	udelay(1);
	source = readl(xbar->regs + 0x800);
	read_clock = readl(xbar->regs + 0x808);
	output = readl(xbar->regs + 0x814);
	/* Native takeConnectionDown logs these intermediate samples and
	 * continues with inverse-clock gates below (9135918..9135a08).
	 * They are not its completion condition. Require the full idle
	 * snapshot after those writes; never release on a stale final status.
	 */
	pr_info("apple-t8132-dpxbar: intermediate stop status source=%#x read=%#x output=%#x\n",
		source, read_clock, output);
	apple_dpxbar_t8132_update(xbar, 8, 4, 0);
	apple_dpxbar_t8132_update(xbar, 0x18, 0x30, 0);
	apple_dpxbar_t8132_update(xbar, 0x28, 0x300, 0);
	apple_dpxbar_t8132_update(xbar, 4, 0, 4);
	apple_dpxbar_t8132_update(xbar, 0x14, 0, 4);
	apple_dpxbar_t8132_update(xbar, 0x24, 0, 0x100);
	/* Native takeConnectionDown leaves bit8 at0x34 set. Hardware in the
	 * retained trial ignores a zero write; do not require an unnecessary
	 * recovery for this native parked value. Clocks/status must be stopped,
	 * and subsequent RTKit AP+IOP quiescence still gates DART teardown.
	 * Restore the reversible selector fields after clocks stop.
	 */
	apple_dpxbar_t8132_update(xbar, 0x30, 0xf00f00, 0);
	if (apple_dpxbar_t8132_idle(xbar))
		return -EIO;
	xbar->selected = false;
	pr_info("apple-t8132-dpxbar: clocks stopped and selector restored; native parked register0x34=%#x\n",
		readl(xbar->regs + 0x34));
	return 0;
}

#endif
