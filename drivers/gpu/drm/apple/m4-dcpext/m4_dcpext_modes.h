/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCPEXT_MODES_H
#define M4_DCPEXT_MODES_H
#include "m4_dcpext_afk.h"

/* Mode data is supplied by DRM's EDID parser, never invented here. */
struct m4_dcpext_mode {
	u32 width, height, clock_khz, htotal, vtotal;
	bool interlaced, doublescan;
};

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
