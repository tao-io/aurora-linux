/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCPEXT_MODES_H
#define M4_DCPEXT_MODES_H
#include "m4_dcpext_afk.h"

/* Mode data is supplied by DRM's EDID parser, never invented here. */
struct m4_dcpext_mode {
	u32 width, height, clock_khz, htotal, vtotal;
	bool interlaced, doublescan;
};

#define M4_DCPEXT_NATIVE_MAX_MODES 128
struct m4_dcpext_native_mode {
	struct m4_dcpext_mode geometry;
	u32 hfront, hsync, vfront, vsync;
	u32 color_id, timing_id;
	bool hpositive, vpositive, preferred;
};

/* Decode the current native TimingElements publication. Only real, uncompressed
 * 8bpc full-range RGB/SDR modes within the transport policy are admitted.
 * No allocation; count is zero on error. IDs belong to this publication only. */
int m4_dcpext_native_modes_parse(const void *blob, u32 bytes,
			       struct m4_dcpext_native_mode *modes,
			       u32 capacity, u32 *count);
/* Match full EDID timing geometry to a uniquely closest native timing. */
int m4_dcpext_native_mode_select(const struct m4_dcpext_native_mode *requested,
				const struct m4_dcpext_native_mode *modes,
				u32 count, u32 *selected);

/* The current hardware policy is two HBR lanes, 8bpc RGB. Mode records are
 * the complete firmware-provided 24-byte records, including reserved fields.
 * The selected record is returned unchanged. Unknown flags are not stripped.
 */
int m4_dcpext_mode_validate(const struct m4_dcpext_mode *mode, u32 *fps_16_16);
int m4_dcpext_mode_select(const struct m4_dcpext_mode *mode,
			 const u8 *timings, u32 count, u8 selected[24]);
/* One linear BGRA plane, caller-owned mapping in both external DARTs. */
int m4_dcpext_surface_validate(u32 width, u32 height, u32 pitch,
			      u64 dva, u64 mapped_bytes);
#endif
