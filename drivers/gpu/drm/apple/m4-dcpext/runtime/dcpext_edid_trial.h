/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* EDID query ABI: Janne Grunau / Asahi dpavservep.c, 666b23ae0b3e.
 * M4 header/offsets independently checked against DCPAVProxy's
 * __handleVarBytesMsg at0xfffffe0009fed688 and copyEDID at0xfffffe0009ff2894.
 * OSData wrapper: Hector Martin / Asahi m1n1 fw/common.py.
 * Full EDID block/header/checksum validation follows DRM EDID conventions.
 * No DMA address is ever accepted from a response.
 */
#ifndef DCPEXT_EDID_TRIAL_H
#define DCPEXT_EDID_TRIAL_H
#define EDID_RAW_MAX 1024
#define EDID_CAP (EDID_RAW_MAX + 8)
#define EDID_PACKET (32 + 64 + 48 + EDID_CAP)
struct edid_trial {
	u16 interface;
	u8 sequence, tag;
	bool opened, pending, received, close_sent;
	u32 bytes;
	u8 raw[EDID_RAW_MAX];
	u8 packet[EDID_PACKET];
	int error;
};
static int edid_trial_write(void *cookie, const u8 *p, u32 bytes);

static int edid_trial_send(struct edid_trial *s, void *cookie, bool query, u8 report)
{
	u32 bytes = query ? EDID_PACKET : 24;
	u8 *p = s->packet;
	if (!s->interface || s->error || s->pending)
		return -EINVAL;
	memset(p, 0, bytes);
	p[0] = s->sequence++;
	put_unaligned_le16(s->interface, p + 2);
	put_unaligned_le32(bytes - 8, p + 4);
	p[16] = query ? 0xc0 : report;
	p[17] = query ? 1 : 0;
	p[18] = query ? 0 : 1;
	if (query) {
		p[25] = ++s->tag;
		put_unaligned_le32(bytes - 32, p + 28);
		put_unaligned_le32(0x10000, p + 32);
		put_unaligned_le32(7, p + 36);
		put_unaligned_le32(48 + EDID_CAP, p + 40);
		put_unaligned_le32(0x69706378, p + 44);
		put_unaligned_le32(EDID_CAP, p + 96);
		s->pending = true;
	}
	return edid_trial_write(cookie, p, bytes);
}

static int edid_trial_response(struct edid_trial *s, const u8 *p, u32 bytes)
{
	u32 used, length, blocks;
	const u8 *data;
	static const u8 header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
	if (!s->pending || s->received || bytes < 32 ||
	    get_unaligned_le32(p + 4) != bytes - 8 ||
	    get_unaligned_le16(p + 2) != s->interface || p[16] != 0xc0 ||
	    p[17] != 2 || p[24] || p[25] != s->tag)
		return -EPROTO;
	if (get_unaligned_le32(p + 28))
		return -EREMOTEIO;
	if (bytes < 144 || bytes > EDID_PACKET ||
	    get_unaligned_le32(p + 32) != 0x10000 ||
	    get_unaligned_le32(p + 36) != 7 ||
	    get_unaligned_le32(p + 40) != bytes - 96 ||
	    get_unaligned_le32(p + 44) != 0x69706378 ||
	    get_unaligned_le64(p + 96) != EDID_CAP)
		return -EPROTO;
	if (get_unaligned_le32(p + 112))
		return -EREMOTEIO;
	if (get_unaligned_le64(p + 128) > EDID_CAP)
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
#endif
