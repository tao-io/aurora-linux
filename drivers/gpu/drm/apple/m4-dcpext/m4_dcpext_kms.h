/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCPEXT_KMS_H
#define M4_DCPEXT_KMS_H
#include "m4_dcpext_connector.h"
struct device;
struct iosys_map;
struct m4_dcpext_kms;

/* The route mutex serializes callbacks/hotplug and hardware commits. Pixels
 * are CPU-readable until present returns; the owner copies them into its
 * retained scanout slots. Success requires matching firmware completion.
 * Disable has mode/pixels NULL; an active mode with pixels NULL presents only
 * the background. Neither operation may free uncertain DMA resources. */
struct m4_dcpext_kms_ops {
	int (*present)(void *cookie, const struct m4_dcpext_native_mode *mode,
		       u64 generation, const struct iosys_map *pixels,
		       u32 source_pitch, bool opaque);
};
struct m4_dcpext_kms *m4_dcpext_kms_create(struct device *dev,
		const struct m4_dcpext_kms_ops *ops, void *cookie,
		struct mutex *route_lock);
struct m4_dcpext_connector *m4_dcpext_kms_connector(struct m4_dcpext_kms *kms);
int m4_dcpext_kms_register(struct m4_dcpext_kms *kms);
void m4_dcpext_kms_unplug(struct m4_dcpext_kms *kms);
#endif
