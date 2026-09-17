/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCPEXT_NATIVE_H
#define M4_DCPEXT_NATIVE_H
#include <linux/types.h>

#define M4_DCPEXT_TAG(c, n)                                  \
	(((u32)(c) << 24) | ((u32)('0' + (n) / 100) << 16) | \
	 ((u32)('0' + (n) / 10 % 10) << 8) | (u32)('0' + (n) % 10))
struct m4_dcpext_native;
struct m4_dcpext_rpc;
struct device;
struct seq_file;
void m4_dcpext_native_analytics_show(struct m4_dcpext_native *dcp,
				     struct seq_file *seq);
struct m4_dcpext_native *m4_dcpext_native_start(struct device *dev,
						struct m4_dcpext_rpc *bridge,
						u32 stride);
int m4_dcpext_native_swap(struct m4_dcpext_native *dcp, const void *surface,
			  u64 dva, u32 width, u32 height);
/* Caller owns the returned snapshot and must kfree it. */
void *m4_dcpext_native_property(struct m4_dcpext_native *dcp, const char *key,
				u32 *size);
int m4_dcpext_native_poll(struct m4_dcpext_native *dcp);
void m4_dcpext_native_destroy(struct m4_dcpext_native *dcp);
#endif
