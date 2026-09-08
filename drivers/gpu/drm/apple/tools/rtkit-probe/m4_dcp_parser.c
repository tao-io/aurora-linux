// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Reuse the existing serialized-property decoder without an apple_dcp object. */
#include "m4_dcp_kms.h"
#define parse m4_property_parse
#define enumerate_modes m4_property_enumerate_modes
#define parse_display_attributes m4_property_display_attributes
#define parse_epic_service_init m4_property_epic_service_init
#define parse_sound_constraints m4_property_sound_constraints
#define parse_sound_mode m4_property_sound_mode
#define parse_system_log_mnits m4_property_system_log_mnits
#include "../../trace.h"
#define trace_iomfb_color_mode(...) do { } while (0)
#define trace_iomfb_timing_mode(...) do { } while (0)
#define trace_avep_sound_mode(...) do { } while (0)
#include "../../parser.c"

int m4_dcp_preferred_mode(const void *blob, u32 size, struct drm_display_mode *mode)
{
	struct dcp_parse_ctx ctx = {};
	struct dcp_display_mode *modes;
	unsigned int count;
	int ret;

	ret = m4_property_parse(blob, size, &ctx);
	if (ret)
		return ret;
	modes = m4_property_enumerate_modes(&ctx, &count, 0, 0, 0);
	if (IS_ERR(modes))
		return PTR_ERR(modes);
	ret = -EINVAL;
	/* The qualified J713 session has one preferred physical panel timing. */
	if (count == 1 && modes[0].mode.hdisplay == 2560 && modes[0].mode.vdisplay == 1664 &&
	    drm_mode_vrefresh(&modes[0].mode) == 60) {
		*mode = modes[0].mode;
		ret = 0;
	}
	kfree(modes);
	return ret;
}
