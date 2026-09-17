/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Serialized secondary endpoint adapter. Uses the same retained private
 * arena as the primary endpoint, never a firmware-owned address fallback.
 */
#include "dcpext_afk_channel.h"
#include "m4_dcpext_iboot.h"
static struct m4_dcpext_afk iboot_channel;
static struct m4_dcpext_iboot iboot_state;
static u64 iboot_next_query_ms, iboot_hold_until_ms, iboot_next_health_ms;
static u64 iboot_next_surface_ms, iboot_alternate_until_ms;
static u64 iboot_surface_submit_ns;
static u64 iboot_unblank_at_ms;
static bool iboot_blank_sent;
static unsigned int blank_acks, unblank_acks;
module_param(blank_acks, uint, 0400);
module_param(unblank_acks, uint, 0400);
static unsigned int surface_submits, surface_acks;
module_param(surface_submits, uint, 0400);
module_param(surface_acks, uint, 0400);
static unsigned int image_progress;
static bool image_observation_done;
module_param(image_progress, uint, 0400);
module_param(image_observation_done, bool, 0400);
module_param_named(iboot_mode_acked, iboot_state.mode_acked, bool, 0400);
module_param_named(iboot_surface_acked, iboot_state.surface_acked, bool, 0400);
static int iboot_result = -ENODATA;
static u32 iboot_fe_error, iboot_sys_error;
module_param(iboot_fe_error, uint, 0400);
module_param(iboot_sys_error, uint, 0400);
module_param(iboot_result, int, 0400);
module_param_named(iboot_hpd, iboot_state.hpd, bool, 0400);
module_param_named(iboot_timings, iboot_state.timings_count, uint, 0400);
module_param_named(iboot_colors, iboot_state.colors_count, uint, 0400);

static int iboot_trial_write(void *cookie, const u8 *p, u32 bytes)
{
	if (bytes >= 48 && get_unaligned_le32(p + 32) == 1) {
		iboot_surface_submit_ns = ktime_get_ns();
		surface_submits++;
		pr_info("m4_dcpext_surface: submit=%u ns=%llu dva=%#llx tag=%u\n",
			surface_submits, iboot_surface_submit_ns, iboot_state.frame_dva, p[25]);
	}
	pr_info("m4_dcpext_session: iBoot TX phase=%u type=%#x op=%u bytes=%u power_dirty=%u\n",
		iboot_state.phase, p[16], bytes >= 48 ? get_unaligned_le32(p + 32) : 0,
		bytes, iboot_state.power_dirty);
	return m4_dcpext_afk_write(&iboot_channel, p, bytes);
}

static const struct m4_dcpext_iboot_ops iboot_ops = { .write = iboot_trial_write };

static int iboot_adapter_allocate(void *cookie, u32 size, u8 **buffer, u64 *dva)
{
	size_t offset = session_used;
	int ret = session_buffer(NULL, 0x23, 0, size, dva);
	if (!ret)
		*buffer = pool + offset;
	return ret;
}

static int iboot_adapter_record(void *cookie, const u8 *p, u32 bytes)
{
	bool health_reply = iboot_state.phase == M4_DCPEXT_IBOOT_HOLD_COUNTS;
	bool blank_reply = iboot_state.phase == M4_DCPEXT_IBOOT_POWER_OFF && iboot_state.blank_requested;
	bool surface_reply = iboot_state.phase == M4_DCPEXT_IBOOT_SURFACE;
	int ret = m4_dcpext_iboot_receive(&iboot_state, p, bytes);
	if (!ret && surface_reply && iboot_state.phase == M4_DCPEXT_IBOOT_HOLD && !iboot_state.pending) {
		u64 ack_ns = ktime_get_ns();
		surface_acks++;
		if (image_blank_cycle && iboot_blank_sent)
			unblank_acks++;
		pr_info("m4_dcpext_surface: ack=%u submit_ns=%llu ack_ns=%llu dva=%#llx tag=%u\n",
			surface_acks, iboot_surface_submit_ns, ack_ns, iboot_state.frame_dva, p[25]);
		iboot_next_surface_ms = session_now(NULL) + 500;
		if (!iboot_alternate_until_ms)
			iboot_alternate_until_ms = session_now(NULL) + 20000;
	}
	if (!ret && blank_reply && iboot_state.phase == M4_DCPEXT_IBOOT_BLANK && !iboot_state.pending) {
		blank_acks++;
		iboot_unblank_at_ms = session_now(NULL) + 1000;
		pr_info("m4_dcpext_session: blank ACK; retain buffers for resume\n");
	}
	if (!ret && health_reply && iboot_state.phase == M4_DCPEXT_IBOOT_HOLD && !iboot_state.pending) {
		iboot_fe_error = readl(frame_regs + 0x100);
		iboot_sys_error = readl(regs + 0x100);
		if (iboot_fe_error || iboot_sys_error || !session_dpxbar_selected ||
		    session_link.rate != 0xa || !(readl(atc_core + 0x7034) & 8) ||
		    session_atc_check_partner())
			ret = -EIO;
		else {
			WRITE_ONCE(image_progress, image_progress + 1);
			iboot_next_health_ms = session_now(NULL) + 1000;
		}
	}
	pr_info("m4_dcpext_session: iBoot RX bytes=%u phase=%u ret=%d HPD=%u timings=%u colors=%u\n",
		bytes, iboot_state.phase, ret, iboot_state.hpd,
		iboot_state.timings_count, iboot_state.colors_count);
	if (ret)
		iboot_state.error = iboot_result = ret;
	if (!ret && iboot_state.phase == M4_DCPEXT_IBOOT_CLOSE &&
	    !iboot_state.pending && !iboot_state.power_dirty)
		iboot_ready_for_route_release = true;
	if (iboot_state.phase == M4_DCPEXT_IBOOT_HPD_WAIT)
		iboot_next_query_ms = 0;
	if (!ret && iboot_state.phase == M4_DCPEXT_IBOOT_HOLD && !iboot_hold_until_ms) {
		iboot_hold_until_ms = session_now(NULL) + image_hold_ms;
		iboot_next_health_ms = session_now(NULL) + 1000;
	}
	return ret;
}

static int iboot_adapter_receive(u64 message)
{
	int ret = m4_dcpext_afk_receive(&iboot_channel, message);
	if (ret)
		iboot_result = ret;
	return ret;
}

static int iboot_adapter_progress(void)
{
	static const struct m4_dcpext_afk_ops ops = {
		.send = session_send, .allocate = iboot_adapter_allocate,
		.record = iboot_adapter_record,
	};
	int ret;
	if (!iboot_probe)
		return 0;
	if (!iboot_channel.started) {
		if (!(session.endpoints[1] & BIT(3)))
			return -ENODEV;
		iboot_state.image_enabled = iboot_image;
		if (iboot_image) {
			if (!frame_published || !frame_registers_match())
				return -EUCLEAN;
			const struct m4_dcpext_mode mode = {
				.width = 1920, .height = 1080, .clock_khz = 148500,
				.htotal = 2200, .vtotal = 1125,
			};
			ret = m4_dcpext_iboot_configure(&iboot_state, &mode,
						      FRAME_STRIDE, FRAME_DVA, FRAME_BYTES);
			if (ret)
				return ret;
		}
		m4_dcpext_afk_init(&iboot_channel, &ops, NULL, 0x23);
		return m4_dcpext_afk_start(&iboot_channel);
	}
	if (iboot_channel.error)
		return iboot_channel.error;
	if (iboot_channel.stopped) {
		if (iboot_state.phase != M4_DCPEXT_IBOOT_STOP ||
		    (iboot_state.power_dirty && !quiesce_with_power))
			return -EUCLEAN;
		iboot_finished = true;
		iboot_result = iboot_state.hpd && iboot_state.timings_count &&
			iboot_state.colors_count ? 0 : -ENODATA;
		return 0;
	}
	if (!iboot_channel.running || iboot_channel.stopping)
		return 0;
	if (quiesce_before_afk_stop && !session.ap_quiesced &&
	    iboot_state.phase == M4_DCPEXT_IBOOT_CLOSE)
		return 0;
	/* m1n1 dcp_shutdown releases DPTX before stopping the remaining AFK
	 * clients. Test this dependency independently of presentation changes. */
	if (iboot_close_after_route && iboot_state.phase == M4_DCPEXT_IBOOT_CLOSE &&
	    (dptx_trial_phase != DPTX_TRIAL_DONE || afk_route_dirty ||
	     afk_display_dirty || afk_hpd_dirty))
		return 0;
	if (((edid_probe && !edid_finished) || (audio_probe && !audio_finished)) &&
	    iboot_state.phase == M4_DCPEXT_IBOOT_POWER_OFF)
		return 0;
	if (iboot_state.phase == M4_DCPEXT_IBOOT_HOLD && session_now(NULL) >= iboot_hold_until_ms) {
		iboot_fe_error = readl(frame_regs + 0x100);
		iboot_sys_error = readl(regs + 0x100);
		pr_info("m4_dcpext_session: image hold ended; FE error=%#x SYS error=%#x (not visible-output proof)\n",
			iboot_fe_error, iboot_sys_error);
		if (iboot_fe_error || iboot_sys_error)
			return -EIO;
		WRITE_ONCE(image_observation_done, true);
		/* Recovery-only native handoff: power ownership stays dirty. */
		iboot_state.phase = quiesce_with_power ? M4_DCPEXT_IBOOT_CLOSE : M4_DCPEXT_IBOOT_POWER_OFF;
	}
	if (image_blank_cycle && !iboot_blank_sent &&
	    iboot_state.phase == M4_DCPEXT_IBOOT_HOLD &&
	    session_now(NULL) >= iboot_hold_until_ms - image_hold_ms + 15000) {
		iboot_blank_sent = true;
		return m4_dcpext_iboot_blank(&iboot_state, &iboot_ops, NULL);
	}
	if (image_blank_cycle && iboot_state.phase == M4_DCPEXT_IBOOT_BLANK &&
	    session_now(NULL) >= iboot_unblank_at_ms) {
		iboot_next_query_ms = 0;
		return m4_dcpext_iboot_unblank(&iboot_state);
	}
	if (image_hold_ms > 6000 && iboot_state.phase == M4_DCPEXT_IBOOT_HOLD &&
	    session_now(NULL) >= iboot_next_health_ms)
		iboot_state.phase = M4_DCPEXT_IBOOT_HOLD_COUNTS;
	if (image_alternate && iboot_state.phase == M4_DCPEXT_IBOOT_HOLD &&
	    !iboot_state.pending && session_now(NULL) >= iboot_next_surface_ms &&
	    session_now(NULL) < iboot_alternate_until_ms) {
		u64 next = iboot_state.frame_dva == FRAME_DVA ? FRAME_DVA + FRAME_BYTES : FRAME_DVA;
		return m4_dcpext_iboot_submit(&iboot_state, &iboot_ops, NULL, next);
	}
	/* Let link callbacks run between bounded status queries. */
	bool present = dptx_monitor_seen && session_link.rate == 0xa &&
		session_dpxbar_selected && afk_hpd_dirty;
	if (iboot_state.phase == M4_DCPEXT_IBOOT_HPD_WAIT && present && !iboot_next_query_ms)
		iboot_next_query_ms = session_now(NULL) + 150;
	ret = m4_dcpext_iboot_progress(&iboot_state, &iboot_ops, NULL,
		present && session_now(NULL) >= iboot_next_query_ms);
	if (ret)
		return ret;
	if (iboot_state.phase >= M4_DCPEXT_IBOOT_HPD_WAIT)
		iboot_ready_for_hpd = true;
	if (iboot_state.phase == M4_DCPEXT_IBOOT_STOP && m4_dcpext_afk_tx_empty(&iboot_channel))
		return m4_dcpext_afk_stop(&iboot_channel);
	return 0;
}

static ssize_t iboot_probe_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset,
		iboot_channel.capture, iboot_channel.capture_size);
}

static const struct file_operations iboot_probe_fops = {
	.owner = THIS_MODULE, .read = iboot_probe_read, .llseek = default_llseek,
};

static int iboot_evidence_publish(struct dentry *directory)
{
	if (!iboot_probe)
		return 0;
	return IS_ERR_OR_NULL(debugfs_create_file("iboot-capture", 0400, directory,
		NULL, &iboot_probe_fops)) ? -ENOMEM : 0;
}
