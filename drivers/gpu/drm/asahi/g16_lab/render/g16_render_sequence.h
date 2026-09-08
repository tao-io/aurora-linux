/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_SEQUENCE_H
#define G16_RENDER_SEQUENCE_H

/* A job fully retires before another uses the shared firmware records. Keep
 * zero (the cleared completion word) out of the hardware stamp sequence.
 * Host accounting remains 64-bit; wire stamps wrap without stopping service. */
static inline u32 g16_render_job_stamp(u64 frame)
{
	return (u32)(frame % 0xffffff + 1) << 8;
}
#endif
