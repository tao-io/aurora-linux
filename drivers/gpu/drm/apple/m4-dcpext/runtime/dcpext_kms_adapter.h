/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Bounded hardware qualification of the maintained DRM device. All RPCs and
 * the polling worker share one route lock. Scanout allocations stay mapped
 * even after unplug; the existing owner alone may release them at quiescence. */
#include <linux/iosys-map.h>
#include "m4_dcpext_kms.h"
static DEFINE_MUTEX(native_route_lock);
static struct m4_dcpext_kms *native_kms;
static bool native_kms_ready, native_kms_finished;
static unsigned int native_kms_frames;
static u64 native_kms_until, native_kms_generation;
static u32 native_kms_color, native_kms_timing;
static bool native_kms_powered;
static bool native_kms_connected;
static unsigned int native_kms_slot;
module_param(native_kms_ready, bool, 0400);
module_param(native_kms_finished, bool, 0400);
module_param(native_kms_frames, uint, 0400);

static int native_kms_present(void *cookie,
		const struct m4_dcpext_native_mode *mode, u64 generation,
		const struct iosys_map *pixels, u32 source_pitch, bool opaque)
{
	u8 surface[M4_DCPEXT_SURFACE_SIZE];
	u32 pitch, width, height;
	void *destination;
	u64 dva;
	int ret;

	lockdep_assert_held(&native_route_lock);
	if (!mode) {
		if (!native_kms_powered)
			return 0;
		ret = m4_dcpext_native_set_power(native_client, false, 0);
		if (!ret)
			native_kms_powered = false;
		return ret;
	}
	if (native_kms_finished || !native_kms_ready || !native_kms_connected || !frame_published ||
	    !frame_registers_match())
		return -ENODEV;
	width = mode->geometry.width;
	height = mode->geometry.height;
	pitch = ALIGN(width * 4, 64);
	ret = m4_dcpext_surface_build(surface, width, height, pitch, opaque);
	if (ret)
		return ret;
	if (native_kms_generation != generation || native_kms_color != mode->color_id ||
	    native_kms_timing != mode->timing_id) {
		ret = m4_dcpext_native_set_mode(native_client, generation,
				mode->color_id, mode->timing_id, pitch);
		if (ret)
			return ret;
		native_kms_generation = generation;
		native_kms_color = mode->color_id;
		native_kms_timing = mode->timing_id;
		native_kms_powered = false;
	}
	if (!native_kms_powered) {
		ret = m4_dcpext_native_set_power(native_client, true, generation);
		if (ret)
			return ret;
		native_kms_powered = true;
	}
	if (!pixels)
		return m4_dcpext_native_swap(native_client, generation, NULL, 0, 0, 0);
	/* First commit uses B because the initial visible bars occupy A. A
	 * matching native completion is required before switching slots again. */
	destination = native_kms_slot ? frame_pixels : frame_pixels_b;
	dva = (FRAME_DVA + (native_kms_slot ? 0 : FRAME_BYTES)) | BIT_ULL(40);
	for (u32 y = 0; y < height; y++) {
		iosys_map_memcpy_from(destination + y * pitch, pixels,
				     (size_t)y * source_pitch, width * 4);
		memset(destination + y * pitch + width * 4, 0, pitch - width * 4);
	}
	dma_wmb();
	ret = m4_dcpext_native_swap(native_client, generation, surface, dva, width, height);
	if (!ret) {
		native_kms_slot ^= 1;
		native_kms_frames++;
	}
	return ret;
}
static const struct m4_dcpext_kms_ops native_kms_ops = { .present = native_kms_present };

static int native_kms_publish(void)
{
	struct m4_dcpext_native_mode *modes;
	struct m4_dcpext_connector *connector;
	u64 generation, connector_generation;
	u32 count;
	int ret;

	if (!native_kms_trial || native_kms || !native_mode_done || !edid_state.received)
		return 0;
	modes = m4_dcpext_native_modes(native_client, &count, &generation);
	if (IS_ERR(modes))
		return PTR_ERR(modes);
	native_kms = m4_dcpext_kms_create(dma_dev, &native_kms_ops, NULL, &native_route_lock);
	if (IS_ERR(native_kms)) {
		ret = PTR_ERR(native_kms);
		native_kms = NULL;
		goto out;
	}
	connector = m4_dcpext_kms_connector(native_kms);
	connector_generation = m4_dcpext_connector_invalidate(connector, true);
	ret = m4_dcpext_connector_publish_native(connector, connector_generation,
		edid_state.raw, edid_state.bytes, modes, count, generation);
	if (ret)
		goto out;
	/* Registration emits the normal DRM device add event. The mode list and
	 * actual monitor identity are already available when userspace probes. */
	if (desktop_requested) {
		ret = dcpext_session_enter_runtime(&session);
		if (ret)
			goto out;
		desktop = true;
		edid_channel.streaming = true;
	}
	native_kms_until = session_now(NULL) + (desktop ? desktop_test_ms : 8000);
	native_kms_connected = true;
	native_kms_ready = true;
	ret = m4_dcpext_kms_register(native_kms);
	if (ret)
		native_kms_ready = false;
	else
		pr_info("m4_dcpext_native: DRM registered with live EDID; %s\n",
			desktop ? "connected-monitor desktop active" : "bounded eight-second KMS qualification");
out:
	kfree(modes);
	return ret;
}
static void native_kms_unplug(void)
{
	if (!native_kms_ready || native_kms_finished)
		return;
	/* Shutdown takes the route lock through present; never call under it. */
	m4_dcpext_kms_unplug(native_kms);
	mutex_lock(&native_route_lock);
	if (session.runtime) {
		/* Start a fresh bounded teardown, never extend it on later polls. */
		dcpext_session_leave_runtime(&session);
		afk_probe_deadline = session_now(NULL) + 6000;
		afk_probe_messages = 0;
	}
	native_kms_finished = true;
	native_route_pending = false;
	mutex_unlock(&native_route_lock);
}

#include "dcpext_hotplug.h"
