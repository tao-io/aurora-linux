/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * DCP AV-service EDID query for the J713 external DCP (endpoint 0x28,
 * service "dispext0:dcpav-service-epic:0", provider AppleDCPDP2HDMI).
 *
 * EDID query ABI: Janne Grunau / Asahi dpavservep.c, 666b23ae0b3e. M4
 * header/offsets independently checked against DCPAVProxy's
 * __handleVarBytesMsg and copyEDID (see DISPLAYPORT-POC.md). OSData wrapper:
 * Hector Martin / Asahi m1n1 fw/common.py. Full EDID block/header/checksum
 * validation follows DRM EDID conventions. No DMA address is ever accepted
 * from a response. Predecessors: tools/m4-dcpext-probe/dcpext_edid_trial.h
 * and dcpext_edid_adapter.h (qualified in edid-read-20260914-1: 256 bytes,
 * two valid block checksums).
 *
 * Layering: this component owns its service selection, sequence and tag
 * (identical framing to m4_dcpext_afk.c) and publishes through ops->write,
 * which the owner maps to m4_dcpext_afk_write() of a channel left in
 * record-only mode (no m4_dcpext_afk_select); the channel's record callback
 * feeds m4_dcpext_edid_receive(). Locking: none of its own; the owner calls
 * it under the channel's serialisation.
 *
 * Bounds: at most eight 128-byte blocks (1024 bytes) inline; the packet is
 * 1,176 bytes with 1,032 bytes of response capacity including the eight-byte
 * OSSerialize OSData wrapper. Overflow is rejected, never truncated.
 */
#ifndef M4_DCPEXT_EDID_H
#define M4_DCPEXT_EDID_H
#include "m4_dcpext_afk.h"

#define M4_DCPEXT_EDID_SERVICE "dispext0:dcpav-service-epic:0"
#define M4_DCPEXT_EDID_RAW_MAX 1024
#define M4_DCPEXT_EDID_CAP (M4_DCPEXT_EDID_RAW_MAX + 8)
#define M4_DCPEXT_EDID_PACKET (32 + 64 + 48 + M4_DCPEXT_EDID_CAP)

struct m4_dcpext_edid_ops {
	int (*write)(void *cookie, const u8 *payload, u32 bytes);
};

struct m4_dcpext_edid {
	u16 interface;		/* 0 until the AV service publication matched */
	u8 sequence, tag;
	bool opened, pending, received, close_sent;
	u32 bytes;
	u8 raw[M4_DCPEXT_EDID_RAW_MAX];
	u8 packet[M4_DCPEXT_EDID_PACKET];
	int error;
};

/* query: group 1 / command 7 EDID request; otherwise the open/close report. */
int m4_dcpext_edid_send(struct m4_dcpext_edid *s, const struct m4_dcpext_edid_ops *ops,
			void *cookie, bool query, u8 report);
/* Strict validation of one response payload (V2 header first). */
int m4_dcpext_edid_response(struct m4_dcpext_edid *s, const u8 *p, u32 bytes);
/* Every payload of the channel: publication selection, interface check,
 * reports retained (0), responses validated; the error is latched.
 */
int m4_dcpext_edid_receive(struct m4_dcpext_edid *s, const u8 *p, u32 bytes);
#endif
