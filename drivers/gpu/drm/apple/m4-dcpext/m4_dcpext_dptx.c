// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* See m4_dcpext_dptx.h for attribution, protocol facts and layering. */
#include "m4_dcpext_dptx.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#else
#include <errno.h>
#include <string.h>
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
#endif

#define DPTX_STATUS_UNSUPPORTED M4_DCPEXT_AFK_STATUS_UNSUPPORTED
#define DPTX_MAGIC 0x69706378

int m4_dcpext_dptx_link_callback(struct m4_dcpext_dptx_link *l,
				 const struct m4_dcpext_dptx_phy_ops *ops, void *cookie,
				 u32 command, const u8 *data, u32 size, u8 *reply)
{
	u32 value, drive[6];
	int ret = 0;

	if (l->fault)
		return -EIO;
	switch (command) {
	case 5: /* Native ATC takes its crossbar connection down before rate change. */
		if (size != 16)
			return -EOPNOTSUPP;
		ret = ops->begin_config(cookie, l);
		break;
	case 6: /* didChangeLinkConfiguration: verify the actual configured PHY. */
		if (size != 16)
			return -EOPNOTSUPP;
		ret = ops->validate_config(cookie, l);
		break;
	case 7: /* getMaxLinkRate: supported software ceiling. */
	case 8: /* getLinkRate: return only a successfully configured rate. */
	case 11: /* getActiveLaneCount: native caches the accepted lane count. */
	case 14: /* getDownspread */
		if (size != 32)
			return -EOPNOTSUPP;
		memcpy(reply, data, size);
		memset(reply, 0, 4);
		memset(reply + 16, 0, 8);
		put_unaligned_le32(command == 7 ? M4_DCPEXT_DPTX_RATE_HBR : command == 8 ? l->rate :
				   command == 11 ? l->lanes : 0, reply + 16);
		return 0;
	case 9: /* setLinkRate; native consumes the byte at service 80. */
		if (size != 32)
			return -EOPNOTSUPP;
		value = get_unaligned_le32(data + 16);
		if ((value != 0 && value != M4_DCPEXT_DPTX_RATE_HBR) || get_unaligned_le32(data + 20))
			return -EOPNOTSUPP;
		ret = ops->set_rate(cookie, l, value);
		break;
	case 12: /* setActiveLaneCount on ATC caches policy; rate owns PHY pair. */
		if (size != 32)
			return -EOPNOTSUPP;
		value = get_unaligned_le32(data + 16);
		if ((value != 0 && value != 2) || get_unaligned_le32(data + 20))
			return -EOPNOTSUPP;
		l->lanes = value;
		break;
	case 15: /* Native AppleATCDPPort::setDownspread is a cached boolean. */
		if (size != 32 || get_unaligned_le64(data + 16))
			return -EOPNOTSUPP;
		break; /* False is also enforced at the real PLL start. */
	case 2: /* getMaxDriveSettings: 12-byte {kind, voltage, preemphasis}. */
		if (size != 32)
			return -EOPNOTSUPP;
		memcpy(reply, data, size);
		memset(reply, 0, 4);
		memset(reply + 16, 0, 12);
		put_unaligned_le32(3, reply + 20);
		put_unaligned_le32(3, reply + 24);
		return 0;
	case 3: /* setDriveSettings: count 80, array of 12-byte settings 96. */
		if (size != 56 || get_unaligned_le32(data + 16) != 2 ||
		    !l->rate || l->lanes != 2)
			return -EOPNOTSUPP;
		for (unsigned int i = 0; i < 6; i++)
			drive[i] = get_unaligned_le32(data + 32 + i * 4);
		for (unsigned int i = 0; i < 6; i += 3)
			if (drive[i] || drive[i + 1] > 3 || drive[i + 2] > 3 ||
			    drive[i + 1] + drive[i + 2] > 3)
				return -EOPNOTSUPP;
		ret = ops->set_drive(cookie, l, drive);
		if (!ret)
			memcpy(l->drive, drive, sizeof(l->drive));
		break;
	case 4: /* getDriveSettings: count 64, status 80, output array 96. */
		if (size != 56 || get_unaligned_le32(data) != 2 ||
		    !l->rate || l->lanes != 2)
			return -EOPNOTSUPP;
		memcpy(reply, data, size);
		memset(reply + 16, 0, 4);
		for (unsigned int i = 0; i < 6; i++)
			put_unaligned_le32(l->drive[i], reply + 32 + i * 4);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
	if (ret) {
		l->fault = true;
		return ret;
	}
	memcpy(reply, data, size);
	memset(reply, 0, 4);
	return 0;
}

int m4_dcpext_dptx_display_hints(struct m4_dcpext_monitor *m, const u8 *data, u32 size)
{
	u32 width, height;
	unsigned int length = 0;

	if (size != 160)
		return -EPROTO;
	if (!(get_unaligned_le32(data) & 1))
		return 0; /* Retain the last identified monitor across HPD-off. */
	width = get_unaligned_le32(data + 56);
	height = get_unaligned_le32(data + 60);
	if (!width || width > 16384 || !height || height > 16384)
		return -EPROTO;
	while (length < 16 && data[92 + length]) {
		if (data[92 + length] < 0x20 || data[92 + length] > 0x7e)
			return -EPROTO;
		length++;
	}
	if (!length || length == 16)
		return -EPROTO;
	memcpy(m->name, data + 92, length);
	m->name[length] = 0;
	m->width = width;
	m->height = height;
	m->seen = true;
	return 0;
}

static u32 dptx_command(const struct m4_dcpext_dptx *d)
{
	if (d->phase == M4_DCPEXT_DPTX_HPD_ON || d->phase == M4_DCPEXT_DPTX_HPD_OFF)
		return 8;
	return d->phase == M4_DCPEXT_DPTX_REQUEST ? 6 :
	       d->phase == M4_DCPEXT_DPTX_RELEASE ? 7 : 11;
}

static u32 dptx_group(const struct m4_dcpext_dptx *d)
{
	/* Native displayRelease sets service group 8, unlike older Linux. */
	return d->phase == M4_DCPEXT_DPTX_RELEASE ||
	       d->phase == M4_DCPEXT_DPTX_HPD_ON || d->phase == M4_DCPEXT_DPTX_HPD_OFF ? 8 : 0;
}

static u32 dptx_data_size(const struct m4_dcpext_dptx *d)
{
	return d->phase == M4_DCPEXT_DPTX_REQUEST ||
	       d->phase == M4_DCPEXT_DPTX_RELEASE ? 16 : 32;
}

int m4_dcpext_dptx_send(struct m4_dcpext_dptx *d, const struct m4_dcpext_dptx_ops *ops, void *cookie)
{
	u8 packet[128] = { 0 };
	u32 data_size = dptx_data_size(d), bytes = 96 + data_size;
	int ret;

	if (d->phase == M4_DCPEXT_DPTX_HPD_ON) {
		ret = ops->check_partner(cookie);
		if (ret || !ops->hpd(cookie))
			return ret ? ret : -ENOLINK;
	}
	packet[0] = d->sequence++;
	put_unaligned_le16(d->interface, packet + 2);
	put_unaligned_le32(bytes - 8, packet + 4);
	packet[16] = M4_DCPEXT_AFK_TYPE_COMMAND;
	packet[17] = 1;
	packet[25] = d->phase + 1;
	put_unaligned_le32(64 + data_size, packet + 28);
	put_unaligned_le32(dptx_group(d) << 16, packet + 32);
	put_unaligned_le32(dptx_command(d), packet + 36);
	put_unaligned_le32(data_size, packet + 40);
	put_unaligned_le32(DPTX_MAGIC, packet + 44);
	if (d->phase == M4_DCPEXT_DPTX_CONNECT) {
		put_unaligned_le32(M4_DCPEXT_DPTX_TARGET_ATC1, packet + 100);
		d->route_dirty = true; /* Conservative on publication failure. */
	} else if (d->phase == M4_DCPEXT_DPTX_REQUEST) {
		d->display_dirty = true;
	} else if (d->phase == M4_DCPEXT_DPTX_HPD_ON) {
		put_unaligned_le32(1, packet + 112);
		d->hpd_dirty = true;
	}
	return ops->write(cookie, packet, bytes);
}

int m4_dcpext_dptx_callback(struct m4_dcpext_dptx *d, const struct m4_dcpext_dptx_ops *ops,
			    void *cookie, const u8 *p, u32 bytes)
{
	u8 reply[256] = { 0 };
	u32 response_bytes = 32, command, data_size;
	int ret = -EOPNOTSUPP;

	/* AFKEPCommandRemote::getRspHeaders keeps the incoming flags/tag,
	 * replaces capacity with status, and prepends the common response
	 * header. Only inline commands are qualified.
	 */
	if (bytes < 32 || p[16] != M4_DCPEXT_AFK_TYPE_COMMAND || p[24] ||
	    ++d->callbacks > (d->training ? 128 : 32))
		return -EOPNOTSUPP;
	reply[0] = d->sequence++;
	put_unaligned_le16(d->interface, reply + 2);
	put_unaligned_le32(24, reply + 4);
	memcpy(reply + 8, p + 8, 8);
	reply[16] = p[16];
	reply[17] = 2;
	reply[25] = p[25];
	put_unaligned_le32(DPTX_STATUS_UNSUPPORTED, reply + 28);
	if (d->aux_callbacks && bytes >= 96 && bytes <= sizeof(reply) &&
	    (!get_unaligned_le32(p + 32) ||
	     (d->training && get_unaligned_le32(p + 32) == 0x10000 &&
	      (get_unaligned_le32(p + 36) == 3 || get_unaligned_le32(p + 36) == 4))) &&
	    get_unaligned_le32(p + 44) == DPTX_MAGIC) {
		command = get_unaligned_le32(p + 36);
		data_size = get_unaligned_le32(p + 40);
		if (bytes != 96 + data_size || get_unaligned_le32(p + 28) < 64 + data_size)
			return -EPROTO;
		if ((command == 0 || command == 1) && data_size == 16)
			ret = ops->aux(cookie, command == 0);
		else if ((command == 18 || command == 26) && data_size == 32)
			ret = 0;
		else if (command == 10 && data_size == 32)
			ret = ops->check_partner(cookie);
		else if (command == 21 && data_size == 160)
			ret = m4_dcpext_dptx_display_hints(&d->monitor, p + 96, data_size);
		else if (d->capabilities && command == 7 && data_size == 32)
			ret = ops->check_partner(cookie);
		else if (d->capabilities && ((command == 13 && data_size == 48) ||
			 (command == 20 && data_size == 16)))
			ret = 0;
		if (ret == -EOPNOTSUPP && d->training)
			ret = m4_dcpext_dptx_link_callback(&d->link, ops->phy, cookie, command,
							   p + 96, data_size, reply + 96);
		if (!ret) {
			response_bytes = bytes;
			memcpy(reply + 32, p + 32, 64);
			if (command == 21) {
				/* Notification accepted, without allocating a display or
				 * claiming a completed physical link. Native status 208.
				 */
				memcpy(reply + 96, p + 96, data_size);
				put_unaligned_le32(0, reply + 240);
			}
			/* Native callback status at service 64, boolean at 80. */
			if (command == 18)
				put_unaligned_le32(1, reply + 112);
			if (command == 10)
				put_unaligned_le32(2, reply + 112); /* two DP lanes in negotiated pin D */
			if (command == 7) {
				/* Asahi dptxep.c's HBR3 ceiling advertises the trial
				 * policy, not a configured/trained rate; with training
				 * the HBR ceiling. Native writes u64 at service 80.
				 */
				put_unaligned_le32(d->training ? M4_DCPEXT_DPTX_RATE_HBR : 0x1e, reply + 112);
			}
			if (command == 13) {
				/* Native M4: input rate at service 64, status 80, bool 96.
				 * Preserve input/padding; disable optional spread spectrum
				 * as in the Asahi client. Never claim SSC configured.
				 */
				memcpy(reply + 96, p + 96, data_size);
				put_unaligned_le32(0, reply + 112);
				reply[128] = 0;
			}
			/* 20 is an inactive-sink notification: acknowledge receipt
			 * without changing PHY state or claiming an active sink.
			 */
			put_unaligned_le32(0, reply + 28);
			put_unaligned_le32(response_bytes - 8, reply + 4);
			return ops->write(cookie, reply, response_bytes);
		}
		if (ret != -EOPNOTSUPP) {
			/* A failed PHY operation cannot be acknowledged as success.
			 * Keep the recovery obligation even if firmware later releases.
			 */
			d->phy_fault = true;
			return ret;
		}
	}
	d->result = -EOPNOTSUPP;
	return ops->write(cookie, reply, response_bytes);
}

int m4_dcpext_dptx_receive(struct m4_dcpext_dptx *d, const struct m4_dcpext_dptx_ops *ops,
			   void *cookie, const u8 *p, u32 bytes)
{
	u32 status, data_size, target;

	if (p[17] == 1)
		return m4_dcpext_dptx_callback(d, ops, cookie, p, bytes);
	if (p[17] != 2)
		return 0;
	if (d->phase == M4_DCPEXT_DPTX_DONE || !d->sent || d->replied ||
	    bytes < 32 || p[16] != M4_DCPEXT_AFK_TYPE_COMMAND || p[24] || p[25] != d->phase + 1)
		return -EPROTO;
	d->replied = true;
	status = get_unaligned_le32(p + 28);
	if (status) {
		d->result = -EREMOTEIO;
		return 0;
	}
	data_size = dptx_data_size(d);
	if (bytes != 96 + data_size || get_unaligned_le32(p + 32) != dptx_group(d) << 16 ||
	    get_unaligned_le32(p + 36) != dptx_command(d) ||
	    get_unaligned_le32(p + 40) != data_size || get_unaligned_le32(p + 44) != DPTX_MAGIC)
		return -EPROTO;
	if (d->phase == M4_DCPEXT_DPTX_CONNECT || d->phase == M4_DCPEXT_DPTX_DISCONNECT) {
		target = d->phase == M4_DCPEXT_DPTX_CONNECT ? M4_DCPEXT_DPTX_TARGET_ATC1 : 0;
		if (get_unaligned_le32(p + 96) || get_unaligned_le32(p + 100) != target)
			return -EPROTO;
	}
	status = get_unaligned_le32(p + (dptx_command(d) == 11 ? 112 : 96));
	if (status) {
		d->result = -EREMOTEIO;
		return 0;
	}
	if (d->result == -ENODATA)
		d->result = 0;
	if (d->phase == M4_DCPEXT_DPTX_RELEASE)
		d->display_dirty = false;
	if (d->phase == M4_DCPEXT_DPTX_DISCONNECT)
		d->route_dirty = false;
	if (d->phase == M4_DCPEXT_DPTX_HPD_OFF)
		d->hpd_dirty = false;
	return 0;
}

int m4_dcpext_dptx_progress(struct m4_dcpext_dptx *d, const struct m4_dcpext_dptx_ops *ops,
			    void *cookie)
{
	if (d->phase == M4_DCPEXT_DPTX_HPD_WAIT) {
		if (ops->now_ms(cookie) < d->observe_until)
			return 0;
		if (d->iboot_gate && !d->iboot_finished)
			return 0; /* The session deadline still bounds a failed side service. */
		d->phase = M4_DCPEXT_DPTX_HPD_OFF;
		d->sent = d->replied = false;
	}
	if (d->replied && d->phase != M4_DCPEXT_DPTX_DONE) {
		if (d->phase == M4_DCPEXT_DPTX_CONNECT && d->result)
			d->phase = M4_DCPEXT_DPTX_DISCONNECT;
		else if (d->phase == M4_DCPEXT_DPTX_REQUEST && d->hpd_routing && !d->result)
			d->phase = M4_DCPEXT_DPTX_HPD_ON;
		else if (d->phase == M4_DCPEXT_DPTX_HPD_ON) {
			d->phase = M4_DCPEXT_DPTX_HPD_WAIT;
			d->observe_until = ops->now_ms(cookie) + M4_DCPEXT_DPTX_HPD_OBSERVE_MS;
		} else if (d->phase == M4_DCPEXT_DPTX_HPD_OFF)
			d->phase = M4_DCPEXT_DPTX_RELEASE;
		else
			d->phase++;
		if (d->phase != M4_DCPEXT_DPTX_DONE)
			d->sent = d->replied = false;
	}
	if (!d->sent && d->phase != M4_DCPEXT_DPTX_DONE && d->phase != M4_DCPEXT_DPTX_HPD_WAIT) {
		if (d->iboot_gate && d->phase == M4_DCPEXT_DPTX_HPD_ON && !d->iboot_ready)
			return 0;
		d->sent = true;
		return m4_dcpext_dptx_send(d, ops, cookie);
	}
	return 0;
}
