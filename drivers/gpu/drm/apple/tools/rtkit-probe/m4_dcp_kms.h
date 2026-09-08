/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCP_KMS_H
#define M4_DCP_KMS_H
#include <linux/types.h>
struct m4_dcp_native;
struct drm_display_mode;
int m4_dcp_preferred_mode(const void *blob, u32 size, struct drm_display_mode *mode);
int m4_dcp_kms_register(struct m4_dcp_native *native);
#endif
