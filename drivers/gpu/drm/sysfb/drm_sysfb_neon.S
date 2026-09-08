/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

/* Normal memory only; caller owns the kernel SIMD context.
 * Convert four little-endian XRGB8888 pixels per iteration, replicating
 * each channel's high two bits into the low two output bits. */
SYM_FUNC_START(drm_sysfb_neon_xrgb2101010)
	movi	v4.4s, #0xff
	movi	v5.4s, #0xff, lsl #8
	movi	v6.4s, #0xff, lsl #16
	mov	w3, #0x0c03
	movk	w3, #0x0030, lsl #16
	dup	v7.4s, w3
1:
	cmp	w2, #4
	b.lo	2f
	ldr	q0, [x1], #16
	and	v1.16b, v0.16b, v4.16b
	and	v2.16b, v0.16b, v5.16b
	and	v3.16b, v0.16b, v6.16b
	shl	v1.4s, v1.4s, #2
	shl	v2.4s, v2.4s, #4
	shl	v3.4s, v3.4s, #6
	orr	v0.16b, v1.16b, v2.16b
	orr	v0.16b, v0.16b, v3.16b
	ushr	v1.4s, v0.4s, #8
	and	v1.16b, v1.16b, v7.16b
	orr	v0.16b, v0.16b, v1.16b
	str	q0, [x0], #16
	sub	w2, w2, #4
	b	1b
2:
	cbz	w2, 4f
3:
	ldr	w4, [x1], #4
	and	w5, w4, #0xff
	and	w6, w4, #0xff00
	and	w4, w4, #0xff0000
	lsl	w5, w5, #2
	orr	w5, w5, w6, lsl #4
	orr	w5, w5, w4, lsl #6
	lsr	w6, w5, #8
	and	w6, w6, w3
	orr	w5, w5, w6
	str	w5, [x0], #4
	subs	w2, w2, #1
	b.ne	3b
4:
	ret
SYM_FUNC_END(drm_sysfb_neon_xrgb2101010)
