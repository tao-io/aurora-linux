// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include "m4_dcpext_modes.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#else
#include <errno.h>
#include <string.h>
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
#endif

int m4_dcpext_surface_validate(u32 width, u32 height, u32 pitch,
			      u64 dva, u64 mapped_bytes)
{
	u64 bytes;

	/* Scope is the qualified external controller's linear SDR path. */
	if (!width || width > 1920 || !height || height > 1080 ||
	    pitch < width * 4 || (pitch & 63) || !dva || (dva & 0x3fff))
		return -EINVAL;
	bytes = (u64)pitch * height;
	if (bytes > mapped_bytes || dva >= (1ULL << 36) ||
	    bytes > (1ULL << 36) - dva)
		return -ERANGE;
	return 0;
}

int m4_dcpext_mode_validate(const struct m4_dcpext_mode *m, u32 *fps_16_16)
{
	u64 fps, total;

	if (!m || !fps_16_16 || !m->width ||
	    m->width > 1920 || !m->height || m->height > 1080 || !m->clock_khz ||
	    m->htotal <= m->width || m->vtotal <= m->height ||
	    m->htotal > 65535 || m->vtotal > 65535)
		return -EINVAL;
	if (m->interlaced || m->doublescan)
		return -EOPNOTSUPP;
	/* 2 * 2.7 Gbit/s * 8/10, divided by 24 wire bits per RGB pixel. */
	if ((u64)m->clock_khz * 24 > 4320000)
		return -ERANGE;
	total = (u64)m->htotal * m->vtotal;
	fps = ((u64)m->clock_khz * 1000 * 65536 + total / 2) / total;
	if (fps < 24 * 65536 || fps > 120 * 65536)
		return -ERANGE;
	*fps_16_16 = fps;
	return 0;
}

int m4_dcpext_mode_select(const struct m4_dcpext_mode *m,
			 const u8 *timings, u32 count, u8 selected[24])
{
	const u8 *best = NULL;
	u64 fps, best_delta = ~(u64)0;

	int ret;
	u32 rate;
	bool ambiguous = false;

	if (!timings || !selected || count > 64)
		return -EINVAL;
	ret = m4_dcpext_mode_validate(m, &rate);
	if (ret)
		return ret;
	fps = rate;
	for (u32 i = 0; i < count; i++) {
		const u8 *r = timings + i * 24;
		u32 reported = get_unaligned_le32(r + 12);
		u64 delta = fps > reported ? fps - reported : reported - fps;

		if (get_unaligned_le32(r) != 1 ||
		    get_unaligned_le32(r + 4) != m->width ||
		    get_unaligned_le32(r + 8) != m->height ||
		    get_unaligned_le32(r + 16) || get_unaligned_le32(r + 20))
			continue;
		/* Captured DMT records use nominal integer rates: e.g. 800x600
		 * 60.316 Hz -> 60, 640x480 72.809 Hz -> 72. Prefer the closest
		 * reported rate; fractional 59.94 and 60 remain distinguishable.
		 */
		if (delta >= 65536 || delta > best_delta)
			continue;
		if (delta < best_delta)
			ambiguous = false;
		else if (best && memcmp(best, r, 24))
			ambiguous = true;
		best = r;
		best_delta = delta;
	}
	if (!best)
		return -ENODATA;
	if (ambiguous)
		return -ENOTUNIQ;
	memcpy(selected, best, 24);
	return 0;
}
