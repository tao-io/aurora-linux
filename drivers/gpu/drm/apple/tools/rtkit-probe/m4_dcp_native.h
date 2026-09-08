/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCP_NATIVE_H
#define M4_DCP_NATIVE_H

#define M4_DCP_TAG(c, n) (((u32)(c) << 24) | ((u32)('0' + (n) / 100) << 16) | \
			 ((u32)('0' + (n) / 10 % 10) << 8) | (u32)('0' + (n) % 10))
struct m4_dcp_native;
struct m4_dcp_bridge;
struct device;
struct m4_dcp_native *m4_dcp_native_start(struct device *dev, struct m4_dcp_bridge *bridge);
int m4_dcp_native_swap(struct m4_dcp_native *dcp, const void *surface, u64 dva,
		       u32 width, u32 height);
#endif
