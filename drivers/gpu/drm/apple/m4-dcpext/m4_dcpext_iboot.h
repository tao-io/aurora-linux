/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * iBoot display service client for the J713 external DCP (endpoint 0x23,
 * publication "disp0-service"): mode-count/HPD baseline, wait for external
 * readiness, power, timing/color record lists, mode set, surface submission,
 * health re-query, power-off, close.
 *
 * Command payloads: Hector Martin / Asahi m1n1 dcp_iboot.c and iboot.py
 * (MIT), retaining their distinct 16-byte request / 8-byte response headers;
 * layer extension per Janne Grunau (m1n1 v13.3, 254a2acf1896). The V2
 * envelope is the independently qualified M4 getModeCount framing. Full
 * captured 24-byte timing/color records are preserved. Predecessors:
 * tools/m4-dcpext-probe/dcpext_iboot_trial.h and dcpext_iboot_adapter.h
 * (qualified in afk-iboot-training-20260914-3 and iboot-image-20260914-5).
 *
 * Protocol facts kept here: successful power, mode and surface calls return
 * the 32-byte status-only AFK envelope (no iBoot response body); firmware
 * must report external HPD and nonzero timing/color counts BEFORE power-on
 * (m1n1 display.c ordering), otherwise it manufactures an unrelated mode
 * list; the layer is 172 bytes (164 + the 8-byte extension), the setSurface
 * request 188 bytes before the 32-byte envelope; at most 64 records each.
 *
 * Limits carried over from the PoC fixture, to be replaced by validated
 * framebuffer state before KMS use: the selected mode is the advertised
 * 1920x1080@60 timing with the RGB SDR 32-bit color record; the surface is
 * one BGRA plane, stride 7,680, at frame_dva in both scanout DARTs.
 * Acknowledgement of setSurface is a submission ack, not a vblank or
 * permission to free the previous framebuffer.
 *
 * Layering and locking as in m4_dcpext_edid.h: own selection/sequence/tag,
 * ops->write onto a record-only AFK channel, caller-serialised.
 */
#ifndef M4_DCPEXT_IBOOT_H
#define M4_DCPEXT_IBOOT_H
#include "m4_dcpext_afk.h"

#define M4_DCPEXT_IBOOT_RECORDS 64
#define M4_DCPEXT_IBOOT_RECORD 24
#define M4_DCPEXT_IBOOT_COUNT_ATTEMPTS 10

enum m4_dcpext_iboot_phase {
	M4_DCPEXT_IBOOT_SERVICE, M4_DCPEXT_IBOOT_BASELINE, M4_DCPEXT_IBOOT_POWER_ON,
	M4_DCPEXT_IBOOT_HPD_WAIT, M4_DCPEXT_IBOOT_COUNTS, M4_DCPEXT_IBOOT_TIMINGS,
	M4_DCPEXT_IBOOT_COLORS, M4_DCPEXT_IBOOT_SET_MODE, M4_DCPEXT_IBOOT_SURFACE,
	M4_DCPEXT_IBOOT_HOLD, M4_DCPEXT_IBOOT_HOLD_COUNTS, M4_DCPEXT_IBOOT_POWER_OFF,
	M4_DCPEXT_IBOOT_CLOSE, M4_DCPEXT_IBOOT_STOP, M4_DCPEXT_IBOOT_DONE,
	M4_DCPEXT_IBOOT_BLANK,
};

struct m4_dcpext_iboot_ops {
	int (*write)(void *cookie, const u8 *payload, u32 bytes);
};

struct m4_dcpext_iboot {
	enum m4_dcpext_iboot_phase phase;
	u16 interface;
	u8 sequence, tag;
	bool opened, pending, power_dirty, hpd;
	bool image_enabled, mode_acked, surface_acked;
	bool blank_requested;
	u64 frame_dva;
	u8 selected_mode[48];
	u32 operation, timings_count, colors_count, count_attempts;
	u8 timings[M4_DCPEXT_IBOOT_RECORDS * M4_DCPEXT_IBOOT_RECORD];
	u8 colors[M4_DCPEXT_IBOOT_RECORDS * M4_DCPEXT_IBOOT_RECORD];
	int error;		/* Sticky; power_dirty survives it */
};

/* Advances one step when nothing is pending; monitor_present gates the
 * count re-query while waiting for external HPD (caller spaces at most
 * M4_DCPEXT_IBOOT_COUNT_ATTEMPTS queries, 150 ms apart in the PoC).
 */
int m4_dcpext_iboot_progress(struct m4_dcpext_iboot *s, const struct m4_dcpext_iboot_ops *ops,
			     void *cookie, bool monitor_present);
/* Every payload of the channel: publication selection, reports retained. */
int m4_dcpext_iboot_receive(struct m4_dcpext_iboot *s, const u8 *p, u32 bytes);
/* Picks the qualified timing/color pair into selected_mode. */
int m4_dcpext_iboot_select_mode(struct m4_dcpext_iboot *s);
/* Replace the qualified surface while powered and idle. The owner must map
 * the full surface in both DARTs and retain all submitted buffers. A reply
 * acknowledges submission only; this API does not establish retirement.
 */
int m4_dcpext_iboot_submit(struct m4_dcpext_iboot *s,
			   const struct m4_dcpext_iboot_ops *ops,
			   void *cookie, u64 dva);
/* Keep the service and all buffers owned while powered off. Unblank re-reads
 * HPD/counts and timing/color records, then restores the mode and surface.
 * Neither the power reply nor this API proves DMA retirement.
 */
int m4_dcpext_iboot_blank(struct m4_dcpext_iboot *s,
			  const struct m4_dcpext_iboot_ops *ops, void *cookie);
int m4_dcpext_iboot_unblank(struct m4_dcpext_iboot *s);
#endif
