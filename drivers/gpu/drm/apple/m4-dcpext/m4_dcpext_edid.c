// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* See m4_dcpext_edid.h for attribution, bounds and layering. */
#include "m4_dcpext_edid.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#else
#include <errno.h>
#include <string.h>
static u16 get_unaligned_le16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
#endif

int m4_dcpext_edid_send(struct m4_dcpext_edid *s, const struct m4_dcpext_edid_ops *ops,
			void *cookie, bool query, u8 report)
{
	u32 bytes = query ? M4_DCPEXT_EDID_PACKET : M4_DCPEXT_AFK_REPORT;
	u8 *p = s->packet;

	if (!s->interface || s->error || s->pending)
		return -EINVAL;
	memset(p, 0, bytes);
	p[0] = s->sequence++;
	put_unaligned_le16(s->interface, p + 2);
	put_unaligned_le32(bytes - 8, p + 4);
	p[16] = query ? M4_DCPEXT_AFK_TYPE_COMMAND : report;
	p[17] = query ? 1 : 0;
	p[18] = query ? 0 : 1;
	if (query) {
		p[25] = ++s->tag;
		put_unaligned_le32(bytes - 32, p + 28);
		put_unaligned_le32(0x10000, p + 32);	/* group 1 */
		put_unaligned_le32(7, p + 36);		/* command 7 */
		put_unaligned_le32(48 + M4_DCPEXT_EDID_CAP, p + 40);
		put_unaligned_le32(0x69706378, p + 44);
		put_unaligned_le32(M4_DCPEXT_EDID_CAP, p + 96);
		s->pending = true;
	}
	return ops->write(cookie, p, bytes);
}

int m4_dcpext_edid_response(struct m4_dcpext_edid *s, const u8 *p, u32 bytes)
{
	u32 used, length, blocks;
	const u8 *data;
	static const u8 header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };

	if (!s->pending || s->received || bytes < 32 ||
	    get_unaligned_le32(p + 4) != bytes - 8 ||
	    get_unaligned_le16(p + 2) != s->interface || p[16] != M4_DCPEXT_AFK_TYPE_COMMAND ||
	    p[17] != 2 || p[24] || p[25] != s->tag)
		return -EPROTO;
	if (get_unaligned_le32(p + 28))
		return -EREMOTEIO;
	if (bytes < 144 || bytes > M4_DCPEXT_EDID_PACKET ||
	    get_unaligned_le32(p + 32) != 0x10000 ||
	    get_unaligned_le32(p + 36) != 7 ||
	    get_unaligned_le32(p + 40) != bytes - 96 ||
	    get_unaligned_le32(p + 44) != 0x69706378 ||
	    get_unaligned_le64(p + 96) != M4_DCPEXT_EDID_CAP)
		return -EPROTO;
	if (get_unaligned_le32(p + 112))
		return -EREMOTEIO;
	if (get_unaligned_le64(p + 128) > M4_DCPEXT_EDID_CAP)
		return -E2BIG;
	used = get_unaligned_le64(p + 128);
	if (used < 8 + 128 || used > bytes - 144)
		return -EMSGSIZE;
	data = p + 144;
	length = used - 8;
	if (get_unaligned_le32(data) != 0xd3 ||
	    get_unaligned_le32(data + 4) != (0x8a000000 | length))
		return -EPROTO;
	data += 8;
	if (length % 128 || memcmp(data, header, 8))
		return -EBADMSG;
	blocks = (u32)data[126] + 1;
	if (blocks * 128 != length)
		return -EMSGSIZE;
	for (u32 i = 0; i < blocks; i++) {
		u8 sum = 0;

		for (u32 j = 0; j < 128; j++)
			sum += data[i * 128 + j];
		if (sum)
			return -EBADMSG;
	}
	memcpy(s->raw, data, length);
	s->bytes = length;
	s->received = true;
	s->pending = false;
	return 0;
}

int m4_dcpext_edid_receive(struct m4_dcpext_edid *s, const u8 *p, u32 bytes)
{
	u16 interface;
	int ret;

	if (bytes < M4_DCPEXT_AFK_REPORT || get_unaligned_le32(p + 4) != bytes - 8)
		return -EPROTO;
	interface = get_unaligned_le16(p + 2);
	if (p[16] == M4_DCPEXT_AFK_TYPE_PUBLISH && !p[17]) {
		ret = m4_dcpext_afk_publication_named(p, bytes, interface, M4_DCPEXT_EDID_SERVICE);
		if (ret <= 0)
			return ret;
		if (!interface || s->interface)
			return -EPROTO;
		s->interface = interface;
		return 0;
	}
	/* An AV endpoint multiplexes several services. Publications for the
	 * other services were deliberately not selected above; their subsequent
	 * reports must not poison our EDID transaction either. Responses still
	 * require the selected interface and matching transaction tag. */
	if (!p[17] && interface != s->interface)
		return 0;
	if (!s->interface || interface != s->interface)
		return -EPROTO;
	if (!p[17])
		return 0; /* Reports retained; no report proves EDID success. */
	ret = m4_dcpext_edid_response(s, p, bytes);
	if (ret)
		s->error = ret;
	return ret;
}
