/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Narrow opt-in DPTX training callback implementation.
 * Native M4 RemotePortProxy message offsets: service starts at raw+32;
 * payload begins at service64/raw96. Exact local callback manifests are
 * retained under build/displayport-kernelcache/dptx-cb-*. Asahi dptxep.h
 * (Sven Peter and contributors) supplies callback names/indices.
 *
 * PoC policy: HBR only, two lanes, SSC off, native fallback drive table.
 * Unsupported requests are explicit. The hardware adapter must implement
 * dptx_link_set_rate() and dptx_link_set_drive() with real PHY writes,
 * retain ownership on a failed operation, and stop the PHY before teardown.
 */
#ifndef DCPEXT_DPTX_LINK_H
#define DCPEXT_DPTX_LINK_H
struct dptx_link_state {
	u32 rate, lanes;
	bool fault;
	u32 drive[6]; /* Two successfully written {kind, voltage, preemphasis}. */
};

static int dptx_link_set_rate(struct dptx_link_state *s, u32 rate);
static int dptx_link_begin_config(struct dptx_link_state *s);
static int dptx_link_set_drive(struct dptx_link_state *s, const u32 drive[6]);
static int dptx_link_validate_config(struct dptx_link_state *s);

/* Returns Unsupported for unknown/unsupported operations, without touching
 * the supplied reply or hardware. The caller owns framing and failure latch.
 */
static int dptx_link_callback(struct dptx_link_state *s, u32 command,
			      const u8 *data, u32 size, u8 *reply)
{
	u32 value, drive[6];
	int ret = 0;
	if (s->fault)
		return -EIO;
	switch (command) {
	case 5: /* Native ATC takes its crossbar connection down before rate change. */
		if (size != 16)
			return -EOPNOTSUPP;
		ret = dptx_link_begin_config(s);
		break;
	case 6: /* didChangeLinkConfiguration: verify the actual configured PHY. */
		if (size != 16)
			return -EOPNOTSUPP;
		ret = dptx_link_validate_config(s);
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
		put_unaligned_le32(command == 7 ? 0xa : command == 8 ? s->rate :
				   command == 11 ? s->lanes : 0, reply + 16);
		return 0;
	case 9: /* setLinkRate; native consumes the byte at service80. */
		if (size != 32)
			return -EOPNOTSUPP;
		value = get_unaligned_le32(data + 16);
		if ((value != 0 && value != 0xa) || get_unaligned_le32(data + 20))
			return -EOPNOTSUPP;
		ret = dptx_link_set_rate(s, value);
		break;
	case 12: /* setActiveLaneCount on ATC caches policy; rate owns PHY pair. */
		if (size != 32)
			return -EOPNOTSUPP;
		value = get_unaligned_le32(data + 16);
		if ((value != 0 && value != 2) || get_unaligned_le32(data + 20))
			return -EOPNOTSUPP;
		s->lanes = value;
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
	case 3: /* setDriveSettings: count80, array of 12-byte settings96. */
		if (size != 56 || get_unaligned_le32(data + 16) != 2 ||
		    !s->rate || s->lanes != 2)
			return -EOPNOTSUPP;
		for (unsigned int i = 0; i < 6; i++)
			drive[i] = get_unaligned_le32(data + 32 + i * 4);
		for (unsigned int i = 0; i < 6; i += 3)
			if (drive[i] || drive[i + 1] > 3 || drive[i + 2] > 3 ||
			    drive[i + 1] + drive[i + 2] > 3)
				return -EOPNOTSUPP;
		ret = dptx_link_set_drive(s, drive);
		if (!ret)
			memcpy(s->drive, drive, sizeof(s->drive));
		break;
	case 4: /* getDriveSettings: count64, status80, output array96. */
		if (size != 56 || get_unaligned_le32(data) != 2 ||
		    !s->rate || s->lanes != 2)
			return -EOPNOTSUPP;
		memcpy(reply, data, size);
		memset(reply + 16, 0, 4);
		for (unsigned int i = 0; i < 6; i++)
			put_unaligned_le32(s->drive[i], reply + 32 + i * 4);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
	if (ret) {
		s->fault = true;
		return ret;
	}
	memcpy(reply, data, size);
	memset(reply, 0, 4);
	return 0;
}
#endif
