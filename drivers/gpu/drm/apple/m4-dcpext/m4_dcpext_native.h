/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCPEXT_NATIVE_H
#define M4_DCPEXT_NATIVE_H
#include <linux/types.h>
#include "m4_dcpext_modes.h"

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
/* Requires selected and powered-on mode from the same hotplug generation. */
int m4_dcpext_native_swap(struct m4_dcpext_native *dcp, u64 generation, const void *surface,
			  u64 dva, u32 width, u32 height);
/* Close the native client and request power state zero. This is NOT a DMA
 * retirement fence: the owner must still establish AP/IOP quiescence. */
int m4_dcpext_native_stop(struct m4_dcpext_native *dcp);
/* Native system-shutdown power flags, still requires AP/IOP ACKs.
 * Select instead of stop(), not after it. Hardware qualification pending. */
int m4_dcpext_native_stop_for_shutdown(struct m4_dcpext_native *dcp);
/* Diagnostic only: service destruction crashes the qualified M4 firmware
 * after a trained session. Not a production teardown or DMA retirement fence. */
int m4_dcpext_native_shutdown(struct m4_dcpext_native *dcp);
/* Caller kfree()s the returned coherent mode snapshot. Errors are ERR_PTRs. */
struct m4_dcpext_native_mode *m4_dcpext_native_modes(struct m4_dcpext_native *dcp,
						  u32 *count, u64 *generation);
/* IDs must come from the specified current snapshot; arbitrary IDs are refused. */
int m4_dcpext_native_set_mode(struct m4_dcpext_native *dcp, u64 generation,
			     u32 color_id, u32 timing_id, u32 pitch);
/* Power-on requires a successfully selected mode from the current generation.
 * Power-off is independent of hotplug generation and never retires DMA. */
int m4_dcpext_native_set_power(struct m4_dcpext_native *dcp, bool on, u64 generation);
/* Caller owns the returned snapshot and must kfree it. */
void *m4_dcpext_native_property(struct m4_dcpext_native *dcp, const char *key,
				u32 *size);
/* External HPD loss invalidates sink identity/modes, never DMA ownership.
 * The owner serializes this with presentation and subsequent HPD requests. */
void m4_dcpext_native_invalidate_sink(struct m4_dcpext_native *dcp);
/* Firmware sink state is independent of an adapter holding physical HPD high. */
int m4_dcpext_native_sink(struct m4_dcpext_native *dcp, bool *connected, u64 *generation);
int m4_dcpext_native_poll(struct m4_dcpext_native *dcp);
void m4_dcpext_native_destroy(struct m4_dcpext_native *dcp);
#endif
