/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Shared per-device sequencer: caller owns MMIO, power and device lock.
 * Independent T8132 TX/RX power fields; guarded standalone HBR cycle tested.
 * Derived register facts from aciophy_{tx,rx}_power{up,down}; exact J713
 * image identity and original Asahi ATC convention credits are recorded in
 * atc-t8132-dp-pll.h and DISPLAYPORT-POC.md. Used by the standalone ATC probe.
 * Power fields alone do not configure DP rate, drive strength or calibration.
 */
#ifndef APPLE_ATCPHY_T8132_LANE_POWER_H
#define APPLE_ATCPHY_T8132_LANE_POWER_H

/* Native samples the whole register, samples the two-bit field again, then
 * combines them. Keep that ordering and its final readback. The final read
 * is not a readiness poll; native uses it only for optional logging.
 */
static void apple_t8132_atc_lane_power_field(void __iomem *regs, u32 offset, unsigned int shift,
				 unsigned int lane, bool enable)
{
	u32 snapshot = readl(regs + offset);
	u32 field = readl(regs + offset);
	u32 mask = 3u << shift, bit = 1u << (shift + lane);
	field = enable ? field | bit : field & ~bit;
	writel((snapshot & ~mask) | (field & mask), regs + offset);
	(void)readl(regs + offset);
}

static int apple_t8132_atc_lane_tx_power(void __iomem *regs, unsigned int lane, bool enable)
{
	if (lane > 1)
		return -EINVAL;
	if (enable) {
		apple_t8132_atc_lane_power_field(regs, 0x10c, 4, lane, true);
		apple_t8132_atc_lane_power_field(regs, 0x10c, 6, lane, true);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 0x10c, 0, lane, true);
		apple_t8132_atc_lane_power_field(regs, 0x10c, 2, lane, true);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 0x10c, 8, lane, false);
		apple_t8132_atc_lane_power_field(regs, 0x10c, 10, lane, true);
		udelay(1);
	} else {
		apple_t8132_atc_lane_power_field(regs, 0x10c, 8, lane, true);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 0x10c, 0, lane, false);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 0x10c, 4, lane, false);
		udelay(1);
	}
	return 0;
}

static int apple_t8132_atc_lane_rx_power(void __iomem *regs, unsigned int lane, bool enable)
{
	if (lane > 1)
		return -EINVAL;
	if (enable) {
		apple_t8132_atc_lane_power_field(regs, 0xc, 4, lane, true);
		apple_t8132_atc_lane_power_field(regs, 0xc, 6, lane, true);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 0xc, 0, lane, true);
		apple_t8132_atc_lane_power_field(regs, 0xc, 2, lane, true);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 8, 10, lane, true);
		apple_t8132_atc_lane_power_field(regs, 8, 12, lane, true);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 8, 6, lane, true);
		apple_t8132_atc_lane_power_field(regs, 8, 8, lane, true);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 8, 14, lane, false);
		apple_t8132_atc_lane_power_field(regs, 8, 16, lane, true);
		udelay(1);
	} else {
		apple_t8132_atc_lane_power_field(regs, 8, 14, lane, true);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 8, 6, lane, false);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 8, 10, lane, false);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 0xc, 0, lane, false);
		udelay(1);
		apple_t8132_atc_lane_power_field(regs, 0xc, 4, lane, false);
		udelay(1);
	}
	return 0;
}
#endif
