// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* See m4_dcpext_iboot.h for attribution, protocol facts and limits. */
#include "m4_dcpext_iboot.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#else
#include <errno.h>
#include <string.h>
static u16 get_unaligned_le16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
#endif

/* iBoot operations (m1n1 dcp_iboot.c): 1 setSurface, 2 setPower,
 * 3 getModeCount, 4 getTimingModes, 5 getColorModes, 6 setMode.
 */
static int iboot_packet(struct m4_dcpext_iboot *s, const struct m4_dcpext_iboot_ops *ops,
			void *cookie, u8 report, u32 op, int power)
{
	u8 packet[220] = { 0 };
	u32 bytes = op == 1 ? 220 : op == 6 ? 96 : op ? (power < 0 ? 48 : 49) : 24;
	int ret;

	packet[0] = s->sequence++;
	put_unaligned_le16(s->interface, packet + 2);
	put_unaligned_le32(bytes - 8, packet + 4);
	packet[16] = op ? M4_DCPEXT_AFK_TYPE_COMMAND : report;
	packet[17] = op ? 1 : 0;
	packet[18] = op ? 0 : 1;
	if (op) {
		packet[25] = ++s->tag;
		put_unaligned_le32(op == 3 ? 20 : op == 4 || op == 5 ? 4096 : 8, packet + 28);
		put_unaligned_le32(op, packet + 32);
		put_unaligned_le32(bytes - 32, packet + 36);
		if (power >= 0)
			packet[48] = power;
		if (op == 6)
			memcpy(packet + 48, s->selected_mode, 48);
		if (op == 1) {
			/* 3 packed 44-byte planes + 32-byte metadata + 8 zeros.
			 * The M4 DCP dispatcher requires command bytes >= 188
			 * (16 + 172); the legacy 164-byte layer only warns.
			 * This matches m1n1's v13_3 layer extension.
			 */
			put_unaligned_le32((u32)s->frame_dva, packet + 48 + 4);
			put_unaligned_le32((u32)(s->frame_dva >> 32), packet + 48 + 8);
			put_unaligned_le32(7680, packet + 48 + 16);
			put_unaligned_le32(1, packet + 48 + 36); /* planar */
			put_unaligned_le32(1, packet + 48 + 136); /* one plane */
			put_unaligned_le32(1920, packet + 48 + 140);
			put_unaligned_le32(1080, packet + 48 + 144);
			put_unaligned_le32(1, packet + 48 + 148); /* BGRA */
			put_unaligned_le32(1, packet + 48 + 152); /* sRGB */
			put_unaligned_le32(1, packet + 48 + 156); /* SDR */
		}
		s->operation = op;
		s->pending = true;
		if (op == 2 && power)
			s->power_dirty = true; /* Power-on may reach firmware even on send error. */
	}
	ret = ops->write(cookie, packet, bytes);
	if (ret)
		s->error = ret;
	return ret;
}

int m4_dcpext_iboot_receive(struct m4_dcpext_iboot *s, const u8 *p, u32 bytes)
{
	u32 count;
	u16 interface;
	int ret;

	if (s->error)
		return s->error;
	if (bytes < M4_DCPEXT_AFK_REPORT || get_unaligned_le32(p + 4) != bytes - 8)
		return -EPROTO;
	interface = get_unaligned_le16(p + 2);
	if (p[16] == M4_DCPEXT_AFK_TYPE_PUBLISH && !p[17]) {
		ret = m4_dcpext_afk_publication_named(p, bytes, interface, NULL);
		if (ret <= 0)
			return ret;
		if (!interface || s->interface || s->phase != M4_DCPEXT_IBOOT_SERVICE)
			return -EPROTO;
		s->interface = interface;
		return 0;
	}
	if (!s->interface || interface != s->interface)
		return -EPROTO;
	if (!p[17])
		return 0; /* Retained raw report; no report proves power/command success. */
	if (bytes < M4_DCPEXT_AFK_HEADER || p[16] != M4_DCPEXT_AFK_TYPE_COMMAND || p[17] != 2 ||
	    p[24] || !s->pending || p[25] != s->tag)
		return -EPROTO;
	if (get_unaligned_le32(p + 28))
		return -EREMOTEIO;
	if (s->phase == M4_DCPEXT_IBOOT_SET_MODE || s->phase == M4_DCPEXT_IBOOT_SURFACE) {
		if (!s->image_enabled || !s->power_dirty || bytes != 32 ||
		    s->operation != (s->phase == M4_DCPEXT_IBOOT_SET_MODE ? 6 : 1))
			return -EPROTO;
		if (s->phase == M4_DCPEXT_IBOOT_SET_MODE) {
			s->mode_acked = true;
			s->phase = M4_DCPEXT_IBOOT_SURFACE;
		} else {
			s->surface_acked = true;
			s->phase = M4_DCPEXT_IBOOT_HOLD;
		}
		s->pending = false;
		return 0;
	}
	if (s->phase == M4_DCPEXT_IBOOT_POWER_ON || s->phase == M4_DCPEXT_IBOOT_POWER_OFF) {
		/* Captured M4 setPower reply is ONLY the matched AFK status
		 * envelope. There is no iBoot response header/payload. m1n1's
		 * dcp_ib_set_power likewise checks the command status alone.
		 */
		if (bytes != 32 || s->operation != 2)
			return -EPROTO;
		if (s->phase == M4_DCPEXT_IBOOT_POWER_OFF) {
			s->power_dirty = false;
			s->phase = s->blank_requested ? M4_DCPEXT_IBOOT_BLANK : M4_DCPEXT_IBOOT_CLOSE;
		} else {
			s->phase = M4_DCPEXT_IBOOT_TIMINGS;
		}
		s->pending = false;
		return 0;
	}
	if (bytes < 40 || get_unaligned_le32(p + 32) != s->operation ||
	    get_unaligned_le32(p + 36) != bytes - 32)
		return -EPROTO;
	switch (s->phase) {
	case M4_DCPEXT_IBOOT_BASELINE:
	case M4_DCPEXT_IBOOT_COUNTS:
	case M4_DCPEXT_IBOOT_HOLD_COUNTS:
		if (bytes != 52 || s->operation != 3 || p[40] > 1)
			return -EPROTO;
		if (get_unaligned_le32(p + 44) > M4_DCPEXT_IBOOT_RECORDS ||
		    get_unaligned_le32(p + 48) > M4_DCPEXT_IBOOT_RECORDS)
			return -E2BIG;
		if (s->phase == M4_DCPEXT_IBOOT_HOLD_COUNTS) {
			/* Health reply: HPD and both counts must still match. */
			if (!s->surface_acked || !s->power_dirty || !p[40] ||
			    get_unaligned_le32(p + 44) != s->timings_count ||
			    get_unaligned_le32(p + 48) != s->colors_count)
				return -EIO;
			s->phase = M4_DCPEXT_IBOOT_HOLD;
			break;
		}
		if (s->phase == M4_DCPEXT_IBOOT_BASELINE) {
			s->phase = M4_DCPEXT_IBOOT_HPD_WAIT;
		} else {
			s->hpd = p[40];
			s->timings_count = get_unaligned_le32(p + 44);
			s->colors_count = get_unaligned_le32(p + 48);
			/* Asahi m1n1 display.c waits for external HPD and nonzero
			 * mode counts BEFORE power-on. Otherwise firmware selects
			 * a stub interface and manufactures unrelated mode lists.
			 */
			s->count_attempts++;
			s->phase = s->hpd && s->timings_count && s->colors_count ?
				M4_DCPEXT_IBOOT_POWER_ON :
				s->count_attempts < M4_DCPEXT_IBOOT_COUNT_ATTEMPTS ?
				M4_DCPEXT_IBOOT_HPD_WAIT : M4_DCPEXT_IBOOT_CLOSE;
		}
		break;
	case M4_DCPEXT_IBOOT_TIMINGS:
	case M4_DCPEXT_IBOOT_COLORS:
		if (bytes < 44 || s->operation != (s->phase == M4_DCPEXT_IBOOT_TIMINGS ? 4 : 5))
			return -EPROTO;
		count = get_unaligned_le32(p + 40);
		if (count > M4_DCPEXT_IBOOT_RECORDS || bytes != 44 + M4_DCPEXT_IBOOT_RECORD * count ||
		    count != (s->phase == M4_DCPEXT_IBOOT_TIMINGS ? s->timings_count : s->colors_count))
			return -EPROTO;
		for (u32 i = 0; i < count; i++)
			if (get_unaligned_le32(p + 44 + i * M4_DCPEXT_IBOOT_RECORD) > 1)
				return -EPROTO;
		memcpy(s->phase == M4_DCPEXT_IBOOT_TIMINGS ? s->timings : s->colors, p + 44,
		       count * M4_DCPEXT_IBOOT_RECORD);
		s->phase = s->phase == M4_DCPEXT_IBOOT_TIMINGS ? M4_DCPEXT_IBOOT_COLORS :
			s->image_enabled ? M4_DCPEXT_IBOOT_SET_MODE : M4_DCPEXT_IBOOT_POWER_OFF;
		break;
	default:
		return -EPROTO;
	}
	s->pending = false;
	return 0;
}

int m4_dcpext_iboot_submit(struct m4_dcpext_iboot *s,
			   const struct m4_dcpext_iboot_ops *ops,
			   void *cookie, u64 dva)
{
	if (s->error)
		return s->error;
	if (s->pending || s->phase != M4_DCPEXT_IBOOT_HOLD)
		return -EBUSY;
	if (!s->opened || !s->image_enabled || !s->mode_acked ||
	    !s->surface_acked || !s->power_dirty || !s->hpd)
		return -EUCLEAN;
	if (!dva || (dva & 0x3fff) || dva > (1ULL << 36) - 7680 * 1080)
		return -EINVAL;
	s->frame_dva = dva;
	s->surface_acked = false;
	s->phase = M4_DCPEXT_IBOOT_SURFACE;
	return iboot_packet(s, ops, cookie, 0, 1, -1);
}

int m4_dcpext_iboot_blank(struct m4_dcpext_iboot *s,
			  const struct m4_dcpext_iboot_ops *ops, void *cookie)
{
	if (s->error)
		return s->error;
	if (s->pending || s->phase != M4_DCPEXT_IBOOT_HOLD)
		return -EBUSY;
	if (!s->opened || !s->image_enabled || !s->power_dirty ||
	    !s->mode_acked || !s->surface_acked)
		return -EUCLEAN;
	s->blank_requested = true;
	s->phase = M4_DCPEXT_IBOOT_POWER_OFF;
	return iboot_packet(s, ops, cookie, 0, 2, 0);
}

int m4_dcpext_iboot_unblank(struct m4_dcpext_iboot *s)
{
	if (s->error)
		return s->error;
	if (s->pending || s->phase != M4_DCPEXT_IBOOT_BLANK)
		return -EBUSY;
	if (!s->opened || !s->image_enabled || s->power_dirty || !s->blank_requested)
		return -EUCLEAN;
	s->blank_requested = false;
	s->hpd = s->mode_acked = s->surface_acked = false;
	s->timings_count = s->colors_count = s->count_attempts = 0;
	memset(s->timings, 0, sizeof(s->timings));
	memset(s->colors, 0, sizeof(s->colors));
	memset(s->selected_mode, 0, sizeof(s->selected_mode));
	s->phase = M4_DCPEXT_IBOOT_HPD_WAIT;
	return 0;
}

int m4_dcpext_iboot_select_mode(struct m4_dcpext_iboot *s)
{
	const u8 *timing = NULL, *color = NULL;

	if (!s->image_enabled || !s->frame_dva || s->frame_dva > (1ULL << 36) - 7680 * 1080 ||
	    (s->frame_dva & 0x3fff) || !s->hpd || !s->power_dirty ||
	    s->timings_count > M4_DCPEXT_IBOOT_RECORDS || s->colors_count > M4_DCPEXT_IBOOT_RECORDS)
		return -EINVAL;
	/* Qualified fixture: timing (1,1920,1080,60<<16,0,0), color (1,1,1,1,32,0). */
	for (u32 i = 0; i < s->timings_count; i++) {
		const u8 *p = s->timings + i * M4_DCPEXT_IBOOT_RECORD;

		if (get_unaligned_le32(p) == 1 && get_unaligned_le32(p + 4) == 1920 &&
		    get_unaligned_le32(p + 8) == 1080 && get_unaligned_le32(p + 12) == (60U << 16) &&
		    !get_unaligned_le32(p + 16) && !get_unaligned_le32(p + 20)) {
			timing = p;
			break;
		}
	}
	for (u32 i = 0; i < s->colors_count; i++) {
		const u8 *p = s->colors + i * M4_DCPEXT_IBOOT_RECORD;

		if (get_unaligned_le32(p) == 1 && get_unaligned_le32(p + 4) == 1 &&
		    get_unaligned_le32(p + 8) == 1 && get_unaligned_le32(p + 12) == 1 &&
		    get_unaligned_le32(p + 16) == 32 && !get_unaligned_le32(p + 20)) {
			color = p;
			break;
		}
	}
	if (!timing || !color)
		return -ENODATA;
	memcpy(s->selected_mode, timing, M4_DCPEXT_IBOOT_RECORD);
	memcpy(s->selected_mode + M4_DCPEXT_IBOOT_RECORD, color, M4_DCPEXT_IBOOT_RECORD);
	return 0;
}

int m4_dcpext_iboot_progress(struct m4_dcpext_iboot *s, const struct m4_dcpext_iboot_ops *ops,
			     void *cookie, bool monitor_present)
{
	if (s->error)
		return s->error;
	if (s->pending || !s->interface)
		return 0;
	if (!s->opened) {
		s->opened = true;
		s->phase = M4_DCPEXT_IBOOT_BASELINE;
		return iboot_packet(s, ops, cookie, M4_DCPEXT_AFK_TYPE_OPEN, 0, -1);
	}
	if (s->phase == M4_DCPEXT_IBOOT_HPD_WAIT) {
		if (!monitor_present)
			return 0;
		s->phase = M4_DCPEXT_IBOOT_COUNTS;
	}
	switch (s->phase) {
	case M4_DCPEXT_IBOOT_BASELINE:
	case M4_DCPEXT_IBOOT_COUNTS:
	case M4_DCPEXT_IBOOT_HOLD_COUNTS:
		return iboot_packet(s, ops, cookie, 0, 3, -1);
	case M4_DCPEXT_IBOOT_POWER_ON:
	case M4_DCPEXT_IBOOT_POWER_OFF:
		return iboot_packet(s, ops, cookie, 0, 2, s->phase == M4_DCPEXT_IBOOT_POWER_ON);
	case M4_DCPEXT_IBOOT_TIMINGS:
		return iboot_packet(s, ops, cookie, 0, 4, -1);
	case M4_DCPEXT_IBOOT_COLORS:
		return iboot_packet(s, ops, cookie, 0, 5, -1);
	case M4_DCPEXT_IBOOT_SET_MODE: {
		int ret = m4_dcpext_iboot_select_mode(s);

		if (ret)
			return s->error = ret;
		return iboot_packet(s, ops, cookie, 0, 6, -1);
	}
	case M4_DCPEXT_IBOOT_SURFACE:
		if (!s->mode_acked)
			return s->error = -EUCLEAN;
		return iboot_packet(s, ops, cookie, 0, 1, -1);
	case M4_DCPEXT_IBOOT_CLOSE:
		s->phase = M4_DCPEXT_IBOOT_STOP;
		return iboot_packet(s, ops, cookie, M4_DCPEXT_AFK_TYPE_CLOSE, 0, -1);
	default:
		return 0;
	}
}
