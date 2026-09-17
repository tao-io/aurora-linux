// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/slab.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include "m4_dcpext_connector.h"
#include "m4_dcpext_edid.h"

struct m4_connector_state {
	struct drm_connector_state base;
	u64 generation;
};

static struct m4_dcpext_connector *to_m4(struct drm_connector *connector)
{
	return container_of(connector, struct m4_dcpext_connector, base);
}

static void mode_geometry(const struct drm_display_mode *mode,
			  struct m4_dcpext_mode *geometry)
{
	*geometry = (struct m4_dcpext_mode) {
		.width = mode->hdisplay, .height = mode->vdisplay,
		.clock_khz = mode->clock, .htotal = mode->htotal,
		.vtotal = mode->vtotal,
		.interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE,
		.doublescan = mode->flags & DRM_MODE_FLAG_DBLSCAN,
	};
}

static int select_mode(struct m4_dcpext_connector *c,
		       const struct drm_display_mode *mode, u8 timing[24])
{
	struct m4_dcpext_mode geometry;

	/* Firmware does not accept porch timings here. Only modes produced by
	 * DRM's EDID parser are eligible; never accept arbitrary custom timings
	 * just because their size and rounded refresh match a firmware record.
	 */
	if (mode->vscan > 1 || mode->flags & ~(DRM_MODE_FLAG_PHSYNC |
	    DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC))
		return -EINVAL;
	mode_geometry(mode, &geometry);
	return m4_dcpext_mode_select(&geometry, c->timings,
				    c->timings_count, timing);
}

static bool mode_known(struct m4_dcpext_connector *c,
		       const struct drm_display_mode *mode)
{
	for (u32 i = 0; i < c->modes_count; i++)
		if (drm_mode_equal(mode, &c->modes[i]))
			return true;
	return false;
}

static int get_modes(struct drm_connector *connector)
{
	struct m4_dcpext_connector *c = to_m4(connector);
	struct drm_display_mode *mode, *next, *modes;
	u32 count = 0;
	u8 timing[24];
	int ret;

	mutex_lock(&c->lock);
	kfree(c->modes);
	c->modes = NULL;
	c->modes_count = 0;
	ret = drm_edid_connector_update(connector, c->connected ? c->edid : NULL);
	if (ret || !c->connected || !c->edid)
		goto out;
	drm_edid_connector_add_modes(connector);
	list_for_each_entry_safe(mode, next, &connector->probed_modes, head) {
		if (select_mode(c, mode, timing)) {
			list_del(&mode->head);
			drm_mode_destroy(connector->dev, mode);
		} else {
			count++;
		}
	}
	modes = kcalloc(count, sizeof(*modes), GFP_KERNEL);
	if (!modes) {
		/* mode_valid will reject all modes, including core fallbacks. */
		count = 0;
		goto out;
	}
	c->modes = modes;
	list_for_each_entry(mode, &connector->probed_modes, head)
		drm_mode_copy(&modes[c->modes_count++], mode);
	/* EDID reports sink capability; v1 transports only RGB at eight bpc. */
	connector->display_info.bpc = 8;
	connector->display_info.color_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444);
 out:
	mutex_unlock(&c->lock);
	return count;
}

static enum drm_connector_status detect(struct drm_connector *connector, bool force)
{
	struct m4_dcpext_connector *c = to_m4(connector);
	enum drm_connector_status status;

	mutex_lock(&c->lock);
	status = c->connected ? connector_status_connected : connector_status_disconnected;
	mutex_unlock(&c->lock);
	return status;
}

static enum drm_mode_status mode_valid(struct drm_connector *connector,
				       const struct drm_display_mode *mode)
{
	struct m4_dcpext_connector *c = to_m4(connector);
	bool valid;

	mutex_lock(&c->lock);
	valid = c->connected && c->edid && mode_known(c, mode);
	mutex_unlock(&c->lock);
	return valid ? MODE_OK : MODE_BAD;
}

static void destroy_state(struct drm_connector *connector,
			  struct drm_connector_state *state)
{
	struct m4_connector_state *s = container_of(state, struct m4_connector_state, base);

	__drm_atomic_helper_connector_destroy_state(state);
	kfree(s);
}

static void reset(struct drm_connector *connector)
{
	struct m4_connector_state *s;

	if (connector->state)
		destroy_state(connector, connector->state);
	connector->state = NULL;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (s)
		__drm_atomic_helper_connector_reset(connector, &s->base);
}

static struct drm_connector_state *duplicate_state(struct drm_connector *connector)
{
	struct m4_connector_state *s;

	if (!connector->state)
		return NULL;
	s = kmemdup(container_of(connector->state, struct m4_connector_state, base),
		    sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;
	__drm_atomic_helper_connector_duplicate_state(connector, &s->base);
	return &s->base;
}

static int atomic_check(struct drm_connector *connector, struct drm_atomic_state *state)
{
	struct m4_dcpext_connector *c = to_m4(connector);
	struct drm_connector_state *cs = drm_atomic_get_new_connector_state(state, connector);
	struct drm_crtc_state *crtc;
	struct m4_connector_state *s = container_of(cs, struct m4_connector_state, base);
	int ret = 0;

	if (!cs->crtc) {
		s->generation = 0;
		return 0;
	}
	crtc = drm_atomic_get_new_crtc_state(state, cs->crtc);
	if (!crtc || !crtc->enable)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (!c->connected || !c->edid || !mode_known(c, &crtc->mode))
		ret = -ENODEV;
	else
		s->generation = c->generation;
	mutex_unlock(&c->lock);
	return ret;
}

static void destroy(struct drm_connector *connector)
{
	struct m4_dcpext_connector *c = to_m4(connector);

	/* Owner has stopped connection workers and drained atomic commits. */
	drm_connector_cleanup(connector);
	drm_edid_free(c->edid);
	kfree(c->modes);
}

static const struct drm_connector_funcs connector_funcs = {
	.detect = detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = destroy,
	.reset = reset,
	.atomic_duplicate_state = duplicate_state,
	.atomic_destroy_state = destroy_state,
};

static const struct drm_connector_helper_funcs connector_helpers = {
	.get_modes = get_modes,
	.mode_valid = mode_valid,
	.atomic_check = atomic_check,
};

int m4_dcpext_connector_init(struct drm_device *drm,
			   struct m4_dcpext_connector *c, struct drm_encoder *encoder)
{
	int ret;

	mutex_init(&c->lock);
	c->generation = 1;
	ret = drm_connector_init(drm, &c->base, &connector_funcs, DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		return ret;
	c->base.polled = DRM_CONNECTOR_POLL_HPD;
	c->base.interlace_allowed = false;
	c->base.doublescan_allowed = false;
	drm_connector_helper_add(&c->base, &connector_helpers);
	ret = drm_connector_attach_encoder(&c->base, encoder);
	if (ret)
		drm_connector_cleanup(&c->base);
	return ret;
}

u64 m4_dcpext_connector_invalidate(struct m4_dcpext_connector *c, bool connected)
{
	u64 generation;

	mutex_lock(&c->base.dev->mode_config.mutex);
	mutex_lock(&c->lock);
	generation = ++c->generation;
	c->connected = connected;
	drm_edid_free(c->edid);
	c->edid = NULL;
	kfree(c->modes);
	c->modes = NULL;
	c->modes_count = 0;
	c->timings_count = 0;
	drm_edid_connector_update(&c->base, NULL);
	mutex_unlock(&c->lock);
	mutex_unlock(&c->base.dev->mode_config.mutex);
	return generation;
}

int m4_dcpext_connector_publish(struct m4_dcpext_connector *c, u64 generation,
			      const u8 *raw, u32 bytes, const u8 *timings, u32 count)
{
	const struct drm_edid *edid;
	int ret = 0;

	if (!raw || bytes < 128 || bytes > M4_DCPEXT_EDID_RAW_MAX ||
	    bytes % 128 || bytes != ((u32)raw[126] + 1) * 128 ||
	    !timings || !count || count > 64)
		return -EINVAL;
	edid = drm_edid_alloc(raw, bytes);
	if (!edid)
		return -ENOMEM;
	if (!drm_edid_valid(edid)) {
		drm_edid_free(edid);
		return -EINVAL;
	}
	mutex_lock(&c->base.dev->mode_config.mutex);
	mutex_lock(&c->lock);
	if (!c->connected || c->generation != generation) {
		ret = -ESTALE;
	} else if (c->edid) {
		/* A new snapshot requires invalidation, even on the same cable. */
		ret = -EALREADY;
	} else {
		c->edid = edid;
		edid = NULL;
		memcpy(c->timings, timings, count * 24);
		c->timings_count = count;
	}
	mutex_unlock(&c->lock);
	mutex_unlock(&c->base.dev->mode_config.mutex);
	drm_edid_free(edid);
	return ret;
}

void m4_dcpext_connector_hotplug(struct m4_dcpext_connector *c)
{
	drm_kms_helper_connector_hotplug_event(&c->base);
}

int m4_dcpext_connector_commit_mode(struct m4_dcpext_connector *c,
				  const struct drm_connector_state *state,
				  const struct drm_display_mode *mode,
				  struct m4_dcpext_mode *geometry, u8 timing[24])
{
	const struct m4_connector_state *s;
	int ret;

	if (!state || state->connector != &c->base || !state->crtc || !mode || !geometry || !timing)
		return -EINVAL;
	s = container_of(state, struct m4_connector_state, base);
	mutex_lock(&c->lock);
	if (!c->connected || !c->edid || s->generation != c->generation)
		ret = -ESTALE;
	else if (!mode_known(c, mode))
		ret = -EINVAL;
	else {
		ret = select_mode(c, mode, timing);
		if (!ret)
			mode_geometry(mode, geometry);
	}
	mutex_unlock(&c->lock);
	return ret;
}
