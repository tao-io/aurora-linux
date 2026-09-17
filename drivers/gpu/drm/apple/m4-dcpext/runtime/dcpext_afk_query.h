/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Independently expressed AFKEP V2 framing from the J713 native client.
 * See DISPLAYPORT-POC.md and the retained native-function manifests.
 * getModeCount payload: Asahi m1n1 fw/dcp/iboot.py (MIT).
 * This bounded probe sends open, one query, and close.
 */
static unsigned int afk_query;
module_param(afk_query, uint, 0400);
MODULE_PARM_DESC(afk_query, "0 capture, 1 modes, 2 AV power, 3 validate, 4 route cycle, 5 lifecycle trial, 6 lifecycle with AUX, 7 HPD with flipped pin-D PHY routing");
static u16 afk_query_interface;
static u8 afk_query_sequence;
static bool afk_query_open, afk_query_sent, afk_query_replied;
static bool afk_query_closing, afk_query_closed;
static bool afk_route_disconnect, afk_route_dirty;
static int afk_query_result = -ENODATA;
module_param_named(query_result, afk_query_result, int, 0400);
MODULE_PARM_DESC(query_result, "Command reply status, independent of service and transport teardown");

static bool afk_query_tx_empty(void)
{
	u8 *header = afk_probe_rings[0].header;
	u32 block = afk_probe_rings[0].block;
	u32 rptr, wptr;
	if (!header)
		return false;
	rptr = READ_ONCE(*(u32 *)(header + block));
	wptr = READ_ONCE(*(u32 *)(header + 2 * block));
	dma_rmb();
	return rptr < afk_probe_rings[0].size && !(rptr & (block - 1)) && rptr == wptr;
}

/* Use the maintained publication path, including duplicate-header wrap.
 * The primary PoC still owns its handshake and receive/evidence adapter. */
static int afk_query_write(const u8 *payload, u32 bytes)
{
	u32 next;
	int ret;

	if (!afk_probe_running || afk_probe_stopping || afk_probe_error || bytes > 256)
		return -EINVAL;
	ret = m4_dcpext_afk_ring_write(&afk_probe_rings[0], payload, bytes, &next);
	if (ret)
		return ret;
	pr_info("m4_dcpext_session: AFK V2 TX interface=%u type=%#x category=%u bytes=%u wptr=%u\n",
		afk_query_interface, payload[16], payload[17], bytes, next);
	return afk_probe_send(0xa2, next);
}

#include "dcpext_dptx_trial.h"

static int afk_query_message(u8 type, bool command)
{
	u8 packet[144] = { 0 };
	u32 bytes = command ? (afk_query == 3 ? 144 : afk_query >= 2 ? 128 : 48) : 24;

	packet[0] = afk_query_sequence++;
	put_unaligned_le16(afk_query_interface, packet + 2);
	put_unaligned_le32(bytes - 8, packet + 4);
	packet[16] = type;
	packet[17] = command ? 1 : 0; /* report=0, command=1, response=2 */
	packet[18] = command ? 0 : 1; /* native open/close report flag */
	if (command) {
		packet[25] = 1; /* command tag, no out-of-band buffers */
		if (afk_query == 4) {
			/* Native connectTo uses command 11, 32 data bytes. A zero
			 * target clears CONNECTED. Require its acknowledged response
			 * before permitting resource release, even after quiescence.
			 */
			packet[25] = afk_route_disconnect ? 2 : 1;
			put_unaligned_le32(96, packet + 28);
			put_unaligned_le32(11, packet + 36);
			put_unaligned_le32(32, packet + 40);
			put_unaligned_le32(0x69706378, packet + 44);
			put_unaligned_le32(afk_route_disconnect ? 0 : 0x8010, packet + 100);
			/* Conservative on publication failure: keep the recovery pin. */
			afk_route_dirty = true;
		} else if (afk_query == 3) {
			/* Native RemotePortProxy::validateConnection: group 0,
			 * command 12, 48-byte data; Asahi dptxep uses attributes
			 * 0x100 and target CONNECTED | ATC1 | core0 | die0.
			 * Validation only: do not send connect/request/HPD here.
			 */
			put_unaligned_le32(112, packet + 28);
			put_unaligned_le32(12, packet + 36);
			put_unaligned_le32(48, packet + 40);
			put_unaligned_le32(0x69706378, packet + 44);
			put_unaligned_le32(0x100, packet + 96);
			put_unaligned_le32(0x8010, packet + 100);
		} else if (afk_query == 2) {
			/* Native DCPAVControllerProxy::getPower: group 0, command 7,
			 * 64-byte service header plus 32 bytes. Unlike the old m1n1
			 * group 8/command 9 interface, this is the M4 native payload.
			 */
			put_unaligned_le32(96, packet + 28);
			put_unaligned_le32(7, packet + 36);
			put_unaligned_le32(32, packet + 40);
			put_unaligned_le32(0x69706378, packet + 44);
		} else {
			put_unaligned_le32(20, packet + 28);
			put_unaligned_le32(3, packet + 32); /* getModeCount */
			put_unaligned_le32(16, packet + 36);
		}
	}
	return afk_query_write(packet, bytes);
}

/* Flat OSSerialize dictionaries emitted by the observed V2 publications.
 * Tags/alignment follow m1n1 fw/common.py (MIT). Reject nested objects and
 * malformed bounds; never select a unit from the truncated 32-byte name.
 */
static int afk_query_publication_named(const u8 *p, u32 bytes, u16 interface,
				       const char *target)
{
	u32 count, pos = 64, tag, keylen, type, len;
	const u8 *key, *value;
	bool matched = false, have_id = false, have_name = false;

	if (bytes < 64 || get_unaligned_le32(p + 56) != 0xd3)
		return -EPROTO;
	tag = get_unaligned_le32(p + 60);
	count = tag & 0xffffff;
	if ((tag & 0xff000000) != 0x81000000 || !count || count > 32)
		return -EPROTO;
	if (!target)
		matched = !memcmp(p + 24, "disp0-service\0", 14);
	for (u32 i = 0; i < count; i++) {
		if (pos > bytes || bytes - pos < 4)
			return -EPROTO;
		tag = get_unaligned_le32(p + pos);
		pos += 4;
		keylen = tag & 0xffffff;
		if ((tag & 0xff000000) != 0x09000000 || !keylen || keylen > 64 ||
		    ALIGN(keylen, 4) > bytes - pos)
			return -EPROTO;
		key = p + pos;
		pos += ALIGN(keylen, 4);
		if (bytes - pos < 4)
			return -EPROTO;
		tag = get_unaligned_le32(p + pos);
		pos += 4;
		type = (tag >> 24) & 0x7f;
		len = tag & 0xffffff;
		if (!!(tag & 0x80000000) != (i == count - 1))
			return -EPROTO;
		if (type == 4) {
			if (len != 64)
				return -EPROTO;
			len = 8;
		} else if (type == 11) {
			if (len > 1)
				return -EPROTO;
			len = 0;
		} else if (type != 9 && type != 10) {
			return -EOPNOTSUPP;
		}
		if (len > bytes - pos || ALIGN(len, 4) > bytes - pos)
			return -EPROTO;
		value = p + pos;
		pos += ALIGN(len, 4);
		if (keylen == 12 && !memcmp(key, "interface-id", 12)) {
			if (have_id || type != 4 || get_unaligned_le64(value) != interface)
				return -EPROTO;
			have_id = true;
		} else if (keylen == 14 && !memcmp(key, "interface-name", 14)) {
			if (have_name || type != 9)
				return -EPROTO;
			have_name = true;
			if (target)
				matched = len == strlen(target) && !memcmp(value, target, len);
		}
	}
	return pos == bytes && have_id ? matched : -EPROTO;
}

static int afk_query_publication_for(const u8 *p, u32 bytes, u16 interface, u32 query)
{
	return afk_query_publication_named(p, bytes, interface, query == 1 ? NULL :
		query == 2 ? "dispext0:dcpav-controller-epic:0" :
		"dispext0:dcpdptx-port-epic:0");
}

static int afk_query_receive(const u8 *p, u32 bytes)
{
	u16 interface;
	u32 reply_bytes;
	int ret;

	if (!afk_query)
		return 0;
	if (bytes < 24 || get_unaligned_le32(p + 4) != bytes - 8)
		return -EPROTO;
	interface = get_unaligned_le16(p + 2);
	if (p[16] == 0x11 && p[17] == 0) {
		if (!interface)
			return -EPROTO;
		ret = afk_query_publication_for(p, bytes, interface, afk_query);
		if (ret <= 0)
			return ret;
		if (afk_query_interface)
			return -EPROTO;
		afk_query_interface = interface;
		return 0;
	}
	if (!afk_query_interface || interface != afk_query_interface)
		return -EPROTO;
	if (p[16] == 0x13 && !p[17] && (p[18] & 1) && afk_query_closing) {
		afk_query_closed = true;
		return 0;
	}
	if (afk_query >= 5)
		return dptx_trial_receive(p, bytes);
	if (p[17] != 2)
		return 0; /* Retained raw reports are decoded offline. */
	if (!afk_query_sent || afk_query_replied || bytes < 32 ||
	    p[16] != 0xc0 || p[24] & 1 ||
	    p[25] != (afk_query == 4 && afk_route_disconnect ? 2 : 1))
		return -EPROTO;
	/* Native handleClientResponse matches by the tag, then passes the
	 * common 64-bit field to its callback; it does not require an echo.
	 * The first captured M4 reply filled it despite a zero request field.
	 */
	afk_query_replied = true;
	if (get_unaligned_le32(p + 28)) {
		pr_info("m4_dcpext_session: query %u command status=%#x\n", afk_query, get_unaligned_le32(p + 28));
		afk_query_result = -EREMOTEIO;
		return 0;
	}
	if (afk_query == 4) {
		u32 target = afk_route_disconnect ? 0 : 0x8010;
		if (bytes != 128 || get_unaligned_le32(p + 32) ||
		    get_unaligned_le32(p + 36) != 11 || get_unaligned_le32(p + 40) != 32 ||
		    get_unaligned_le32(p + 44) != 0x69706378 ||
		    get_unaligned_le32(p + 96) || get_unaligned_le32(p + 100) != target)
			return -EPROTO;
		pr_info("m4_dcpext_session: DPTX route target=%#x service_status=%#x disconnect=%u\n",
			target, get_unaligned_le32(p + 112), afk_route_disconnect);
		if (get_unaligned_le32(p + 112)) {
			afk_query_result = -EREMOTEIO;
		} else {
			if (afk_query_result == -ENODATA)
				afk_query_result = 0;
			if (afk_route_disconnect)
				afk_route_dirty = false;
		}
		return 0;
	}
	if (afk_query == 3) {
		if (bytes != 144 || get_unaligned_le32(p + 32) ||
		    get_unaligned_le32(p + 36) != 12 || get_unaligned_le32(p + 40) != 48 ||
		    get_unaligned_le32(p + 44) != 0x69706378 ||
		    get_unaligned_le32(p + 96) != 0x100 || get_unaligned_le32(p + 100) != 0x8010)
			return -EPROTO;
		pr_info("m4_dcpext_session: DPTX ATC1 route validation service_status=%#x result=%#x (not connected, not EDID)\n",
			get_unaligned_le32(p + 112), get_unaligned_le32(p + 128));
		afk_query_result = get_unaligned_le32(p + 112) ? -EREMOTEIO : 0;
		return 0;
	}
	if (afk_query == 2) {
		if (bytes != 128 || get_unaligned_le32(p + 32) ||
		    get_unaligned_le32(p + 36) != 7 || get_unaligned_le32(p + 40) != 32 ||
		    get_unaligned_le32(p + 44) != 0x69706378)
			return -EPROTO;
		if (get_unaligned_le32(p + 96)) {
			afk_query_result = -EREMOTEIO;
			return 0;
		}
		pr_info("m4_dcpext_session: AV controller unit0 power=%llu (native getPower; not EDID)\n",
			get_unaligned_le64(p + 112));
		afk_query_result = 0;
		return 0;
	}
	if (bytes != 52 || get_unaligned_le32(p + 32) != 3)
		return -EPROTO;
	reply_bytes = get_unaligned_le32(p + 36);
	if (reply_bytes != 20 || p[40] > 1)
		return -EPROTO;
	pr_info("m4_dcpext_session: mode-count reply HPD=%u timings=%u colors=%u (not EDID)\n",
		p[40], get_unaligned_le32(p + 44), get_unaligned_le32(p + 48));
	afk_query_result = 0;
	return 0;
}

static int afk_query_progress(void)
{
	int ret;
	if (!afk_probe_running || !afk_query_interface)
		return 0;
	if (!afk_query_open) {
		afk_query_open = true;
		return afk_query_message(0x12, false);
	}
	if (afk_query >= 5) {
		ret = dptx_trial_progress();
		if (ret || (dptx_trial_phase != DPTX_TRIAL_DONE && !dptx_quiesce_handoff))
			return ret;
		if (quiesce_before_afk_stop && !session.ap_quiesced)
			return 0;
	}
	if (afk_query == 4 && afk_query_replied && !afk_route_disconnect) {
		afk_route_disconnect = true;
		afk_query_sent = afk_query_replied = false;
	}
	if (!afk_query_sent) {
		afk_query_sent = true;
		return afk_query_message(0xc0, true);
	}
	if (!afk_query_closing && (afk_query_replied || session_now(NULL) >= afk_probe_deadline - 400)) {
		afk_query_closing = true;
		ret = afk_query_message(0x13, false);
		return ret;
	}
	return 0;
}
