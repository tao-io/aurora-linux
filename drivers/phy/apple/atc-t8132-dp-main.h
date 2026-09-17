/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Shared per-device sequencer: caller owns MMIO, power and device lock.
 * Independent T8132 main-link register sequence.
 * Hardware facts: exact native main_init/main_shutdown and called TX/RX/eq
 * routines identified in atc-t8132-dp-pll.h and the provenance report.
 * Masks were checked using single-read perturbations plus complete native
 * traces from multiple initial patterns. No native executable/source import.
 *
 * HBR cycle tested by the guarded standalone ATC1 probe; no DCP integration.
 * Requires prior common/board calibration and proven physical lane ownership.
 * Caller retains resources on errors. Drive-training callbacks are still
 * required for output. Initial eq below is native preset zero,
 * not negotiated sink drive settings. HBR3's different analog path is omitted.
 */
#ifndef APPLE_ATCPHY_T8132_DP_MAIN_H
#define APPLE_ATCPHY_T8132_DP_MAIN_H
#include "atc-t8132-dp-pll.h"
#include "atc-t8132-lane-power.h"

static void apple_t8132_dp_lane_update(void __iomem *regs, unsigned int lane, u32 offset, u32 mask, u32 value)
{
	apple_t8132_dp_pll_update(regs, offset + lane * 0x7000, mask, value);
}

static void apple_t8132_dp_lane_tx_initial_eq(void __iomem *regs, unsigned int lane)
{
	apple_t8132_dp_lane_update(regs, lane, 0xc050, 0xc00f, 9);
	apple_t8132_dp_lane_update(regs, lane, 0xc0d4, 0x10000000, 0x10000000);
	apple_t8132_dp_lane_update(regs, lane, 0xc0d0, 0x7ffff, 0x1f81);
}

static void apple_t8132_dp_lane_rx_initial_eq(void __iomem *regs, unsigned int lane)
{
	apple_t8132_dp_lane_update(regs, lane, 0x9210, 0xc00f, 9);
	apple_t8132_dp_lane_update(regs, lane, 0x9248, 0x10000000, 0x10000000);
	apple_t8132_dp_lane_update(regs, lane, 0x9244, 0x7ffff, 0x1f81);
}

static void apple_t8132_dp_lane_tx_prepare(void __iomem *regs, unsigned int lane, unsigned int rate)
{
	apple_t8132_atc_lane_tx_power(regs, lane, true);
	writel(0x0aafb800, regs + 0xd044 + lane * 0x7000);
	ndelay(500);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x1000000, 0x1000000);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x4000000, 0x4000000);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x7c0, 0x300);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x6c000, 0);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xc0d0, 0x100000, 0x100000);
	apple_t8132_dp_lane_tx_initial_eq(regs, lane);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x100000, 0x100000);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x400000, 0x400000);
	udelay(1);
	apple_t8132_dp_lane_update(regs, lane, 0xd004, 0x3000000, 0x2000000);
	apple_t8132_dp_lane_update(regs, lane, 0xd004, 0x30000000, 0x30000000);
	apple_t8132_dp_lane_update(regs, lane, 0xd00c, 0x7000000, rate == 6 ? 0x5000000 : 0x4000000);
	apple_t8132_dp_lane_update(regs, lane, 0xd010, 0x600, 0x600);
	apple_t8132_dp_lane_update(regs, lane, 0xd01c, 0xc00000, 0x800000);
}

static void apple_t8132_dp_lane_rx_prepare(void __iomem *regs, unsigned int lane, unsigned int rate)
{
	apple_t8132_dp_lane_update(regs, lane, 0x9160, 1, 1);
	apple_t8132_dp_lane_update(regs, lane, 0x91dc, 0x18000, 0x18000);
	apple_t8132_dp_lane_update(regs, lane, 0xb0a4, 3, 2);
	apple_t8132_dp_lane_update(regs, lane, 0xb0a8, 0x18000, 0x10000);
	apple_t8132_dp_lane_update(regs, lane, 0xb06c, 0x18000, 0x10000);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d4, 0x1f1f80, 0x121180);
	apple_t8132_dp_lane_update(regs, lane, 0xb064, 0xfc00, 0xb800);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d8, 7, 4);
	apple_t8132_dp_lane_update(regs, lane, 0xb0dc, 0x18, 0);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d4, 0xe000, 0xc000);
	ndelay(100);
	apple_t8132_atc_lane_rx_power(regs, lane, true);
	apple_t8132_dp_lane_update(regs, lane, 0xb06c, 0x800, 0x800);
	apple_t8132_dp_lane_update(regs, lane, 0x91d4, 3, 1);
	ndelay(10);
	apple_t8132_dp_lane_update(regs, lane, 0x91d4, 2, 2);
	apple_t8132_dp_lane_update(regs, lane, 0xb06c, 0x600, 0x400);
	ndelay(100);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d4, 0xd0000, 0xd0000);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d8, 3, 1);
	ndelay(500);
	apple_t8132_dp_lane_update(regs, lane, 0xb068, 0x180, 0x180);
	ndelay(500);
	apple_t8132_dp_lane_update(regs, lane, 0xb068, 0x60, 0x60);
	ndelay(500);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d4, 0xc0000, 0);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d8, 3, 3);
	udelay(1);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d8, 3, 2);
	udelay(1);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d8, 3, 0);
	ndelay(750);
	apple_t8132_dp_lane_update(regs, lane, 0xb0a8, 0x60000, 0x60000);
	udelay(1);
	apple_t8132_dp_lane_update(regs, lane, 0xb06c, 0x6000, 0x4000);
	apple_t8132_dp_lane_rx_initial_eq(regs, lane);
	apple_t8132_dp_lane_update(regs, lane, 0xb0a8, 0x380000, rate == 6 ? 0x280000 : 0x200000);
	apple_t8132_dp_lane_update(regs, lane, 0xb0a8, 0xc00000, 0x800000);
	ndelay(50);
	apple_t8132_dp_lane_update(regs, lane, 0xb030, 0xc00, 0xc00);
	ndelay(100);
}

static int apple_t8132_atc_dp_main_prepare(void __iomem *regs, unsigned int pair_mask, unsigned int rate, bool ssc)
{
	int ret;
	if ((pair_mask != 1 && pair_mask != 2) ||
	    (rate != 6 && rate != 10 && rate != 20) || ssc)
		return -EOPNOTSUPP;
	ret = apple_t8132_dp_pll_prepare(regs, pair_mask, rate, ssc);
	if (ret)
		return ret;
	apple_t8132_dp_lane_tx_prepare(regs, pair_mask - 1, rate);
	apple_t8132_dp_lane_rx_prepare(regs, pair_mask - 1, rate);
	apple_t8132_dp_pll_update(regs, 0x7000, 1, 0);
	return 0;
}

static int apple_t8132_atc_dp_main_stop(void __iomem *regs, unsigned int pair_mask)
{
	unsigned int lane;
	if (pair_mask != 1 && pair_mask != 2)
		return -EOPNOTSUPP;
	lane = pair_mask - 1;
	apple_t8132_dp_pll_update(regs, 0x7000, 1, 1);
	apple_t8132_dp_lane_update(regs, lane, 0xd01c, 0xc00000, 0xc00000);
	apple_t8132_dp_lane_update(regs, lane, 0xd010, 0x600, 0x400);
	apple_t8132_dp_lane_update(regs, lane, 0xd004, 0x30000000, 0x20000000);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x100000, 0);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x400000, 0);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x4000000, 0);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x1000000, 0);
	ndelay(250);
	apple_t8132_dp_lane_update(regs, lane, 0xd044, 0x1000, 0);
	apple_t8132_atc_lane_tx_power(regs, lane, false);
	apple_t8132_dp_lane_update(regs, lane, 0xb030, 0xc00, 0x800);
	ndelay(50);
	apple_t8132_dp_lane_update(regs, lane, 0xb0a8, 0xc00000, 0xc00000);
	ndelay(500);
	apple_t8132_dp_lane_update(regs, lane, 0xb0a8, 0x60000, 0x40000);
	apple_t8132_dp_lane_update(regs, lane, 0xb068, 0x60, 0x40);
	ndelay(200);
	apple_t8132_dp_lane_update(regs, lane, 0xb068, 0x180, 0x100);
	ndelay(200);
	apple_t8132_dp_lane_update(regs, lane, 0xb0d4, 0x10000, 0);
	ndelay(100);
	apple_t8132_dp_lane_update(regs, lane, 0xb06c, 0x600, 0x600);
	apple_t8132_dp_lane_update(regs, lane, 0x91d4, 3, 1);
	apple_t8132_dp_lane_update(regs, lane, 0xb06c, 0x800, 0);
	apple_t8132_atc_lane_rx_power(regs, lane, false);
	return apple_t8132_dp_pll_stop_after_lanes(regs);
}
#endif
