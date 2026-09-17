/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Shared per-device sequencer: caller owns MMIO, power and device lock.
 * Independent T8132 DP PLL component; guarded standalone HBR cycle tested.
 * Register facts from AppleT8132TypeCPhy main_init/main_shutdown in J713
 * kernelcache SHA256 f42ad6f51d130141095a94b4749d128d648f2be2ac0c291bf492ac1c64e5875d.
 * Exact manifests, address boundaries and comparison evidence are described
 * in DISPLAYPORT-POC.md. No Apple source or executable code is incorporated.
 * Masked I/O convention follows the Asahi ATC driver (Sven Peter et al.).
 *
 * This covers the init prefix BEFORE lane power-up and shutdown suffix AFTER
 * lane power-down. It is not complete main-link initialization or safe PHY
 * teardown. Caller must own the PHY, supply live calibration/common clocks,
 * sequence lane operations and retain ownership on any error. Failed waits
 * stop immediately; they must not be converted into successful callbacks.
 */
#ifndef APPLE_ATCPHY_T8132_DP_PLL_H
#define APPLE_ATCPHY_T8132_DP_PLL_H

#define DP_PLL_REQUEST 0x2000
#define DP_PLL_STATUS  0x7034

static void apple_t8132_dp_pll_update(void __iomem *regs, u32 offset, u32 mask, u32 value)
{
	writel((readl(regs + offset) & ~mask) | value, regs + offset);
}

static int apple_t8132_dp_pll_ack(void __iomem *regs, u32 expected, unsigned int timeout_us)
{
	u32 status;
	return readl_poll_timeout(regs + DP_PLL_REQUEST, status,
				 ((status >> 1) & 1) == expected, 1, timeout_us);
}

/* Capture the original ACK for both phases, not the current request bit. */
static u32 apple_t8132_dp_pll_request_begin(void __iomem *regs)
{
	u32 old = readl(regs + DP_PLL_REQUEST);
	u32 ack = (old >> 1) & 1;
	writel((old & 0xe0000006) | 0x10000000 | (ack ^ 1),
	       regs + DP_PLL_REQUEST);
	return ack;
}

static int apple_t8132_dp_pll_prepare(void __iomem *regs, unsigned int pair_mask, unsigned int rate, bool ssc)
{
	u32 freq_a, freq_b, freq_c, ack, status;
	int ret;
	/* Four-lane routing and spread spectrum have not been qualified. */
	if ((pair_mask != 1 && pair_mask != 2) || ssc)
		return -EOPNOTSUPP;
	switch (rate) {
	case 0x06: freq_a = 0x1e0e021c; freq_b = 0; freq_c = 0x156600; break;
	case 0x0a: freq_a = 0x1e0e01c2; freq_b = 0x07fffffe; freq_c = 0x155200; break;
	case 0x14: freq_a = 0x1e0e01c2; freq_b = 0x07fffffe; freq_c = 0x554800; break;
	case 0x1e: freq_a = 0x1e0e02a3; freq_b = 0x0bff7ffc; freq_c = 0x564800; break;
	default: return -EOPNOTSUPP;
	}
	apple_t8132_dp_pll_update(regs, 0x2234, 3, 0);
	apple_t8132_dp_pll_update(regs, 0x7000, 0x1ff0, 0x490);
	writel(freq_a, regs + 0x2080);
	writel(freq_b, regs + 0x2084);
	writel(freq_c, regs + 0x2088);
	apple_t8132_dp_pll_update(regs, 0x2208, 0x1f0000, 0x70000);
	apple_t8132_dp_pll_update(regs, 0x2218, 1, 1);
	apple_t8132_dp_pll_update(regs, 0x2200, 8, 8);
	ack = apple_t8132_dp_pll_request_begin(regs);
	udelay(1);
	ret = apple_t8132_dp_pll_ack(regs, ack ^ 1, 3000);
	if (ret)
		return ret;
	udelay(1);
	ret = readl_poll_timeout(regs + DP_PLL_STATUS, status,
				 status & 8, 1, 2000);
	if (ret)
		return ret;
	apple_t8132_dp_pll_update(regs, DP_PLL_REQUEST, 0x1ffffff9, 0x10014000 | ack);
	udelay(1);
	ret = apple_t8132_dp_pll_ack(regs, ack, 500);
	if (ret)
		return ret;
	apple_t8132_dp_pll_update(regs, pair_mask == 1 ? 0xb064 : 0x12064, 0xc, 8);
	apple_t8132_dp_pll_update(regs, 0x7000, 0xc, 0xc);
	udelay(1);
	return 0;
}

/* Entry is native main_shutdown AFTER both selected lane shutdown helpers.
 * Native's initial 0x7000 bit-0 write precedes those helpers and is excluded.
 */
static int apple_t8132_dp_pll_stop_after_lanes(void __iomem *regs)
{
	u32 ack, status;
	int ret;
	apple_t8132_dp_pll_update(regs, 0x7000, 0xc, 8);
	ack = apple_t8132_dp_pll_request_begin(regs);
	ret = apple_t8132_dp_pll_ack(regs, ack ^ 1, 500);
	if (ret)
		return ret;
	apple_t8132_dp_pll_update(regs, DP_PLL_REQUEST, 0x1ffffff9, 0x10000018 | ack);
	ret = apple_t8132_dp_pll_ack(regs, ack, 500);
	if (ret)
		return ret;
	ret = readl_poll_timeout(regs + DP_PLL_STATUS, status,
				 !(status & 8), 1, 5);
	if (ret)
		return ret;
	apple_t8132_dp_pll_update(regs, 0x2200, 8, 0);
	return 0;
}
#endif
