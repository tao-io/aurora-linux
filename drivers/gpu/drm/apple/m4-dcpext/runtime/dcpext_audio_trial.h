/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Martin Povišer / Asahi av.c: audio getElements(group1,cmd16).
 * Independently checked M4 retrieveElements at0xfffffe0009ff9e9c and
 * __handleVarBytesMsg at0xfffffe0009fed688. Metadata only, no DMA/start.
 */
#ifndef DCPEXT_AUDIO_TRIAL_H
#define DCPEXT_AUDIO_TRIAL_H
#define AUDIO_CAP 16384
#define AUDIO_PACKET (144 + AUDIO_CAP)
struct audio_trial {
	u16 interface;
	u8 sequence, tag;
	bool opened, pending, received, close_sent;
	u32 used;
	u8 packet[AUDIO_PACKET];
	u8 elements[AUDIO_CAP];
};
static int audio_trial_write(void *cookie, const u8 *p, u32 bytes);
static int audio_trial_send(struct audio_trial *s, void *cookie, bool query, u8 report)
{
	u32 bytes = query ? AUDIO_PACKET : 24;
	u8 *p = s->packet;
	if (!s->interface || s->pending || (query && s->received))
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
		put_unaligned_le32(16, p + 36);
		put_unaligned_le32(bytes - 96, p + 40);
		put_unaligned_le32(0x69706378, p + 44);
		put_unaligned_le32(AUDIO_CAP, p + 96);
		s->pending = true;
	}
	return audio_trial_write(cookie, p, bytes);
}
static int audio_trial_response(struct audio_trial *s, const u8 *p, u32 bytes)
{
	if (!s->pending || s->received || bytes < 32 ||
	    get_unaligned_le32(p + 4) != bytes - 8 ||
	    get_unaligned_le16(p + 2) != s->interface || p[16] != 0xc0 ||
	    p[17] != 2 || p[24] || p[25] != s->tag)
		return -EPROTO;
	if (get_unaligned_le32(p + 28))
		return -EREMOTEIO;
	if (bytes < 148 || bytes > AUDIO_PACKET ||
	    get_unaligned_le32(p + 32) != 0x10000 ||
	    get_unaligned_le32(p + 36) != 16 ||
	    get_unaligned_le32(p + 40) != bytes - 96 ||
	    get_unaligned_le32(p + 44) != 0x69706378 ||
	    get_unaligned_le64(p + 96) != AUDIO_CAP)
		return -EPROTO;
	if (get_unaligned_le32(p + 112))
		return -EREMOTEIO;
	if (get_unaligned_le64(p + 128) > AUDIO_CAP)
		return -E2BIG;
	u32 used = get_unaligned_le64(p + 128);
	if (used < 8 || used > bytes - 144 || get_unaligned_le32(p + 144) != 0xd3)
		return -EPROTO;
	memcpy(s->elements, p + 144, used);
	s->used = used;
	s->received = true;
	s->pending = false;
	return 0; /* OS object retained for full offline decoding; not playback. */
}
#endif
