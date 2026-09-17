/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Bounded native-M4 display lifecycle characterization. Message layouts come
 * from local J713 RemotePortProxy connectTo/setPowerState/displayRelease
 * extracts; protocol antecedents are Sven Peter / Asahi dptxep.c.
 * Unimplemented PHY callbacks receive Unsupported; implemented callbacks
 * call the guarded ATC component. Display hints retain monitor evidence.
 */
#include "dcpext_display_hints.h"
#include "m4_dcpext_dptx.h"
enum dptx_trial_phase {
	DPTX_TRIAL_CONNECT, DPTX_TRIAL_REQUEST, DPTX_TRIAL_RELEASE,
	DPTX_TRIAL_DISCONNECT, DPTX_TRIAL_DONE,
	DPTX_TRIAL_HPD_ON, DPTX_TRIAL_HPD_WAIT, DPTX_TRIAL_HPD_OFF,
	DPTX_TRIAL_HPD_IRQ,
};
static enum dptx_trial_phase dptx_trial_phase;
static bool release_before_hpd_off;
module_param(release_before_hpd_off, bool, 0400);
static bool afk_display_dirty;
static bool afk_hpd_dirty;
static u64 dptx_trial_observe_until;
static bool native_route_pending;
static bool dptx_runtime_hpd_pending;
static bool dptx_runtime_irq_pending;
static bool dptx_runtime_route_pending;
static unsigned int dptx_trial_callbacks;
static struct m4_dcpext_dptx_errors dptx_errors;
module_param_named(link_error_last, dptx_errors.last_command, uint, 0400);
module_param_array_named(link_error_counts, dptx_errors.count, uint, NULL, 0400);
module_param_named(callback_count, dptx_trial_callbacks, uint, 0400);
static bool link_capabilities;
module_param(link_capabilities, bool, 0400);
MODULE_PARM_DESC(link_capabilities, "Experimental HBR3/downspread capability replies; monitor-only trial leaves them Unsupported");

static u32 dptx_trial_command(void)
{
	if (dptx_trial_phase == DPTX_TRIAL_HPD_IRQ)
		return 9;
	if (dptx_trial_phase == DPTX_TRIAL_HPD_ON || dptx_trial_phase == DPTX_TRIAL_HPD_OFF)
		return 8;
	return dptx_trial_phase == DPTX_TRIAL_REQUEST ? 6 :
	       dptx_trial_phase == DPTX_TRIAL_RELEASE ? 7 : 11;
}

static u32 dptx_trial_group(void)
{
	/* Native displayRelease sets service group 8, unlike older Linux. */
	return dptx_trial_phase == DPTX_TRIAL_HPD_IRQ ||
	       dptx_trial_phase == DPTX_TRIAL_RELEASE ||
	       dptx_trial_phase == DPTX_TRIAL_HPD_ON || dptx_trial_phase == DPTX_TRIAL_HPD_OFF ? 8 : 0;
}

static u32 dptx_trial_data_size(void)
{
	return dptx_trial_phase == DPTX_TRIAL_HPD_IRQ ||
	       dptx_trial_phase == DPTX_TRIAL_REQUEST ||
	       dptx_trial_phase == DPTX_TRIAL_RELEASE ? 16 : 32;
}

static int dptx_trial_send(void)
{
	u8 packet[128] = { 0 };
	u32 data_size = dptx_trial_data_size(), bytes = 96 + data_size;
	int ret;
	if (dptx_trial_phase == DPTX_TRIAL_HPD_ON) {
		ret = session_atc_check_partner();
		if (ret || !session_atc_hpd)
			return ret ? ret : -ENOLINK;
	}
	packet[0] = afk_query_sequence++;
	put_unaligned_le16(afk_query_interface, packet + 2);
	put_unaligned_le32(bytes - 8, packet + 4);
	packet[16] = 0xc0;
	packet[17] = 1;
	packet[25] = dptx_trial_phase + 1;
	put_unaligned_le32(64 + data_size, packet + 28);
	put_unaligned_le32(dptx_trial_group() << 16, packet + 32);
	put_unaligned_le32(dptx_trial_command(), packet + 36);
	put_unaligned_le32(data_size, packet + 40);
	put_unaligned_le32(0x69706378, packet + 44);
	if (dptx_trial_phase == DPTX_TRIAL_CONNECT) {
		put_unaligned_le32(session_atc_target(), packet + 100);
		afk_route_dirty = true;
	} else if (dptx_trial_phase == DPTX_TRIAL_REQUEST) {
		afk_display_dirty = true;
	} else if (dptx_trial_phase == DPTX_TRIAL_HPD_ON) {
		put_unaligned_le32(1, packet + 112);
		afk_hpd_dirty = true;
	}
	pr_info("m4_dcpext_session: DPTX lifecycle phase=%u group=%u command=%u\n",
		dptx_trial_phase, dptx_trial_group(), dptx_trial_command());
	return afk_query_write(packet, bytes);
}

static int dptx_trial_callback(const u8 *p, u32 bytes)
{
	u8 reply[256] = { 0 };
	u32 response_bytes = 32, command, data_size;
	int ret = -EOPNOTSUPP;
	/* AFKEPCommandRemote::getRspHeaders keeps the incoming flags/tag,
	 * replaces capacity with status, and prepends the common response
	 * header. Only inline commands are qualified by this trial.
	 */
	if (bytes < 32 || p[16] != 0xc0 || p[24] ||
	    (!(desktop && session.runtime) && ++dptx_trial_callbacks > (link_training ? 128 : 32)))
		return -EOPNOTSUPP;
	reply[0] = afk_query_sequence++;
	put_unaligned_le16(afk_query_interface, reply + 2);
	put_unaligned_le32(24, reply + 4);
	memcpy(reply + 8, p + 8, 8);
	reply[16] = p[16];
	reply[17] = 2;
	reply[25] = p[25];
	put_unaligned_le32(0xe00002c7, reply + 28); /* native kIOReturnUnsupported */
	if (afk_query >= 6 && bytes >= 96 && bytes <= sizeof(reply) &&
	    (!get_unaligned_le32(p + 32) ||
	     (link_training && get_unaligned_le32(p + 32) == 0x10000 &&
	      (get_unaligned_le32(p + 36) == 3 || get_unaligned_le32(p + 36) == 4))) &&
	    get_unaligned_le32(p + 44) == 0x69706378) {
		command = get_unaligned_le32(p + 36);
		data_size = get_unaligned_le32(p + 40);
		if (bytes != 96 + data_size || get_unaligned_le32(p + 28) < 64 + data_size)
			return -EPROTO;
		if ((command == 0 || command == 1) && data_size == 16)
			ret = session_atc_aux(command == 0);
		else if ((command == 18 || command == 26) && data_size == 32)
			ret = 0;
		else if (command == 10 && data_size == 32)
			ret = session_atc_check_partner();
		else if (command == 21 && data_size == 160)
			ret = dptx_record_display_hints(p + 96, data_size);
		else if (command >= 22 && command <= 24) {
			ret = m4_dcpext_dptx_error_notification(&dptx_errors, command, data_size);
			if (!ret)
				pr_warn("m4_dcpext_session: DPTX link-error notification command=%u count=%u; receipt ACK only, no link-success claim\n",
					command, dptx_errors.count[command - 22]);
		}
		else if (link_capabilities && command == 7 && data_size == 32)
			ret = session_atc_check_partner();
		else if (link_capabilities && ((command == 13 && data_size == 48) ||
			 (command == 20 && data_size == 16)))
			ret = 0;
		if (ret == -EOPNOTSUPP && link_training)
			ret = dptx_link_callback(&session_link, command, p + 96,
						data_size, reply + 96);
		if (!ret) {
			response_bytes = bytes;
			memcpy(reply + 32, p + 32, 64);
			if (command >= 22 && command <= 24) {
				memcpy(reply + 96, p + 96, data_size);
				put_unaligned_le32(0, reply + 96);
			}
			if (command == 21) {
				/* Notification accepted, without allocating a display or
				 * claiming a completed physical link. Native status208.
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
				/* Asahi dptxep.c's HBR3 ceiling. This advertises the
				 * trial policy, not a configured/trained rate. Physical
				 * set-rate/lane callbacks still return Unsupported.
				 * Native handleGetMaxLinkRate writes u64 at service80.
				 */
				put_unaligned_le32(link_training ? 0xa : 0x1e, reply + 112);
			}
			if (command == 13) {
				/* Native M4: input rate at service64, status80, bool96.
				 * Preserve input/padding; disable optional spread spectrum
				 * as in the Asahi client. Never claim SSC configured.
				 */
				memcpy(reply + 96, p + 96, data_size);
				put_unaligned_le32(0, reply + 112);
				reply[128] = 0;
			}
			/* 20 is an inactive-sink notification: acknowledge receipt,
			 * without changing PHY state or claiming an active sink.
			 */
			put_unaligned_le32(0, reply + 28);
			put_unaligned_le32(response_bytes - 8, reply + 4);
			pr_info("m4_dcpext_session: DPTX callback command=%u serviced; AUX active=%u\n",
				command, session_atc_active);
			return afk_query_write(reply, response_bytes);
		}
		if (ret != -EOPNOTSUPP) {
			/* A failed PHY operation cannot be acknowledged as success.
			 * Keep the recovery obligation even if firmware later releases.
			 */
			pr_err("m4_dcpext_session: AUX callback operation failed=%d\n", ret);
			session_atc_fault = true;
			return ret;
		}
	}
	if (bytes >= 96)
		pr_info("m4_dcpext_session: DPTX callback group=%#x command=%u data=%u; explicitly unsupported\n",
			get_unaligned_le32(p + 32), get_unaligned_le32(p + 36),
			get_unaligned_le32(p + 40));
	else
		pr_info("m4_dcpext_session: DPTX callback bytes=%u; explicitly unsupported\n", bytes);
	afk_query_result = -EOPNOTSUPP;
	return afk_query_write(reply, response_bytes);
}

static int dptx_trial_receive(const u8 *p, u32 bytes)
{
	u32 status, data_size, target;
	if (p[17] == 1)
		return dptx_trial_callback(p, bytes);
	if (p[17] != 2)
		return 0;
	if (dptx_trial_phase == DPTX_TRIAL_DONE || !afk_query_sent || afk_query_replied ||
	    bytes < 32 || p[16] != 0xc0 || p[24] || p[25] != dptx_trial_phase + 1)
		return -EPROTO;
	afk_query_replied = true;
	status = get_unaligned_le32(p + 28);
	if (status) {
		pr_info("m4_dcpext_session: DPTX lifecycle phase=%u transport_status=%#x\n",
			dptx_trial_phase, status);
		afk_query_result = -EREMOTEIO;
		return 0;
	}
	data_size = dptx_trial_data_size();
	if (bytes != 96 + data_size || get_unaligned_le32(p + 32) != dptx_trial_group() << 16 ||
	    get_unaligned_le32(p + 36) != dptx_trial_command() ||
	    get_unaligned_le32(p + 40) != data_size || get_unaligned_le32(p + 44) != 0x69706378)
		return -EPROTO;
	if (dptx_trial_phase == DPTX_TRIAL_CONNECT || dptx_trial_phase == DPTX_TRIAL_DISCONNECT) {
		target = dptx_trial_phase == DPTX_TRIAL_CONNECT ? session_atc_target() : 0;
		if (get_unaligned_le32(p + 96) || get_unaligned_le32(p + 100) != target)
			return -EPROTO;
	}
	status = get_unaligned_le32(p + (dptx_trial_command() == 11 ? 112 : 96));
	pr_info("m4_dcpext_session: DPTX lifecycle phase=%u service_status=%#x\n",
		dptx_trial_phase, status);
	if (status) {
		afk_query_result = -EREMOTEIO;
		return 0;
	}
	if (afk_query_result == -ENODATA)
		afk_query_result = 0;
	if (dptx_trial_phase == DPTX_TRIAL_RELEASE)
		afk_display_dirty = false;
	if (dptx_trial_phase == DPTX_TRIAL_DISCONNECT)
		afk_route_dirty = false;
	if (dptx_trial_phase == DPTX_TRIAL_HPD_OFF)
		afk_hpd_dirty = false;
	return 0;
}

/* Keep the service and route alive across sink-only HPD changes. */
static int dptx_runtime_set_hpd(bool on)
{
	if (!desktop || !session.runtime || dptx_runtime_irq_pending || dptx_runtime_hpd_pending || dptx_runtime_route_pending ||
	    dptx_trial_phase != DPTX_TRIAL_HPD_WAIT)
		return -EBUSY;
	dptx_runtime_hpd_pending = true;
	dptx_trial_phase = on ? DPTX_TRIAL_HPD_ON : DPTX_TRIAL_HPD_OFF;
	afk_query_sent = afk_query_replied = false;
	return 0;
}

/* Retarget only after HPD-low. Match dcp_dptx_disconnect/connect: release
 * the display, then connect directly to the next target and request it. The
 * target-zero disconnect belongs to final teardown, not live retargeting. */
static int dptx_runtime_set_route(bool connected)
{
	if (!desktop || !session.runtime || dptx_runtime_irq_pending || dptx_runtime_hpd_pending ||
	    dptx_runtime_route_pending || afk_hpd_dirty ||
	    dptx_trial_phase != DPTX_TRIAL_HPD_WAIT ||
	    (connected && afk_display_dirty))
		return -EBUSY;
	dptx_runtime_route_pending = true;
	dptx_trial_phase = connected ? DPTX_TRIAL_CONNECT : DPTX_TRIAL_RELEASE;
	afk_query_sent = afk_query_replied = false;
	return 0;
}

/* Native J713 AppleDCPDPTXRemotePortProxy::interruptRequestOccurred:
 * service group 8, command 9, 16 zero payload bytes (25G83 constant 73dfa08).
 * An IRQ asks firmware to read the sink's DPCD service vector over AUX. */
static int dptx_runtime_hpd_irq(void)
{
	if (!desktop || !session.runtime || !afk_hpd_dirty ||
	    dptx_runtime_hpd_pending || dptx_runtime_route_pending ||
	    dptx_runtime_irq_pending || dptx_trial_phase != DPTX_TRIAL_HPD_WAIT)
		return -EBUSY;
	dptx_runtime_irq_pending = true;
	dptx_trial_phase = DPTX_TRIAL_HPD_IRQ;
	afk_query_sent = afk_query_replied = false;
	return 0;
}

static int dptx_trial_progress(void)
{
	if (dptx_runtime_irq_pending && afk_query_replied) {
		if (afk_query_result)
			return afk_query_result;
		dptx_runtime_irq_pending = false;
		dptx_trial_phase = DPTX_TRIAL_HPD_WAIT;
		return 0;
	}
	if (dptx_runtime_route_pending && afk_query_replied) {
		if (afk_query_result)
			return afk_query_result;
		if (dptx_trial_phase == DPTX_TRIAL_CONNECT)
			dptx_trial_phase = DPTX_TRIAL_REQUEST;
		else {
			dptx_runtime_route_pending = false;
			dptx_trial_phase = DPTX_TRIAL_HPD_WAIT;
			return 0;
		}
		afk_query_sent = afk_query_replied = false;
	}
	if (dptx_runtime_hpd_pending && afk_query_replied) {
		if (afk_query_result)
			return afk_query_result;
		dptx_runtime_hpd_pending = false;
		dptx_trial_phase = DPTX_TRIAL_HPD_WAIT;
		return 0;
	}
	if (dptx_quiesce_handoff)
		return 0;
	if (dptx_trial_phase == DPTX_TRIAL_HPD_WAIT) {
		if (session_now(NULL) < dptx_trial_observe_until)
			return 0;
		if (native_route_pending)
			return 0; /* Requested native operation, bounded by session deadline. */
		if (iboot_probe && !iboot_finished &&
		    !(iboot_close_after_route && iboot_ready_for_route_release))
			return 0; /* Global session deadline still bounds failed side service. */
		/* Native m1n1 handoff closes channels with the route still owned.
		 * Diagnostic only: leave all dirty obligations set so the owner
		 * must retain resources for watchdog recovery, even after AP ACK. */
		if (quiesce_with_route) {
			dptx_quiesce_handoff = true;
			afk_query_sent = afk_query_replied = true;
			return 0;
		}
		dptx_trial_phase = release_before_hpd_off ? DPTX_TRIAL_RELEASE : DPTX_TRIAL_HPD_OFF;
		afk_query_sent = afk_query_replied = false;
	}
	if (afk_query_replied && dptx_trial_phase != DPTX_TRIAL_DONE) {
		if (dptx_trial_phase == DPTX_TRIAL_CONNECT && afk_query_result)
			dptx_trial_phase = DPTX_TRIAL_DISCONNECT;
		else if (dptx_trial_phase == DPTX_TRIAL_REQUEST && afk_query >= 7 && !afk_query_result)
			dptx_trial_phase = DPTX_TRIAL_HPD_ON;
		else if (dptx_trial_phase == DPTX_TRIAL_HPD_ON) {
			dptx_trial_phase = DPTX_TRIAL_HPD_WAIT;
			dptx_trial_observe_until = session_now(NULL) + 600;
		} else if (dptx_trial_phase == DPTX_TRIAL_HPD_OFF)
			dptx_trial_phase = release_before_hpd_off ? DPTX_TRIAL_DISCONNECT : DPTX_TRIAL_RELEASE;
		else if (dptx_trial_phase == DPTX_TRIAL_RELEASE && release_before_hpd_off && afk_hpd_dirty)
			dptx_trial_phase = DPTX_TRIAL_HPD_OFF;
		else
			dptx_trial_phase++;
		if (dptx_trial_phase != DPTX_TRIAL_DONE)
			afk_query_sent = afk_query_replied = false;
	}
	if (!afk_query_sent && dptx_trial_phase != DPTX_TRIAL_DONE && dptx_trial_phase != DPTX_TRIAL_HPD_WAIT) {
		if (iboot_probe && dptx_trial_phase == DPTX_TRIAL_HPD_ON && !iboot_ready_for_hpd)
			return 0;
		afk_query_sent = true;
		return dptx_trial_send();
	}
	return 0;
}
