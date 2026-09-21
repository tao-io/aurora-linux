/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* AFKEP V2 OOB command: independently derived from M4 native
 * AFKEPCommandLocal::getCmdHeaders / parseResponse,0xfffffe000922f698/f704.
 * Same private-buffer principle as Sven Peter / Asahi EPIC commands.
 * No returned pointer is dereferenced: only the exact owned RX DVA is admitted.
 */
#ifndef DCPEXT_AFK_OOB_H
#define DCPEXT_AFK_OOB_H
#define AFK_OOB_PACKET 56
static int afk_oob_packet_bounded(u8 *out, const u8 *header, u64 tx, u64 rx,
				u32 size, u32 capacity)
{
	if (!tx || !rx || tx == rx || !size || !capacity || capacity > 1024 * 1024 ||
	    size > capacity)
		return -EINVAL;
	memcpy(out, header, 32);
	put_unaligned_le32(48, out + 4);
	out[24] = 1;
	put_unaligned_le32(size, out + 28);
	put_unaligned_le64(rx, out + 32);
	put_unaligned_le64(tx, out + 40);
	put_unaligned_le32(size, out + 48);
	put_unaligned_le32(size, out + 52);
	return 0;
}
static int afk_oob_packet(u8 *out, const u8 *header, u64 tx, u64 rx, u32 size)
{
	return afk_oob_packet_bounded(out, header, tx, rx, size, 32768);
}
static int afk_oob_response(const u8 *p, u32 bytes, u16 interface, u8 tag,
			    u64 rx, u32 capacity, u32 *size)
{
	if (bytes != AFK_OOB_PACKET || get_unaligned_le32(p + 4) != bytes - 8 ||
	    get_unaligned_le16(p + 2) != interface || p[16] != 0xc0 ||
	    p[17] != 2 || p[24] != 1 || p[25] != tag)
		return -EPROTO;
	if (get_unaligned_le32(p + 28))
		return -EREMOTEIO;
	if (get_unaligned_le64(p + 32) != rx)
		return -EPROTO;
	u32 received = get_unaligned_le32(p + 48);
	if (!received || received > capacity)
		return -EMSGSIZE;
	*size = received;
	return 0;
}
#endif
