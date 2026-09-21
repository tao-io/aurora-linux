/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCP_BRIGHTNESS_H
#define M4_DCP_BRIGHTNESS_H

#include <linux/bitops.h>
#include <linux/unaligned.h>

#define M4_PANEL_MAX_NITS 500
#define M4_PANEL_DEFAULT_NITS 120

/* J713 A407: __TEXT 1d554c..1d55a8 copies wire[354] to transaction[799]
 * and the unaligned double at wire[35e] to transaction[7a8]. 2413a8..2413c4
 * converts that double to 16.16 nits. Let firmware use the panel calibration;
 * do not reuse the older firmware's packed iDAC fields or conversion table.
 */
static inline void m4_dcp_encode_brightness(u8 *swap, unsigned int nits)
{
	u64 bits = 0;

	/* Encode an exactly representable integer as IEEE754 binary64 without
	 * using floating point in the kernel. Caller bounds nits to 0..500.
	 */
	if (nits) {
		unsigned int exponent = fls(nits) - 1;

		bits = ((u64)(1023 + exponent) << 52) |
		       (((u64)nits << (52 - exponent)) & ((1ULL << 52) - 1));
	}
	swap[0x354] = 1;
	put_unaligned_le64(bits, swap + 0x35e);
}
#endif
