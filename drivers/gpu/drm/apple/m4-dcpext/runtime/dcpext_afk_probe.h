/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Bounded AFK announcement capture, adapted from Sven Peter's 2022 Asahi
 * afk.c/afk.h at kernel fba91e5d9577. Dynamic block sizing follows m1n1's
 * MIT-licensed proxyclient/m1n1/fw/afk/rbep.py (see provenance hashes).
 * Optional V2 mode-count query is isolated in dcpext_afk_query.h.
 * Unlike the reference receive path, copy evidence before advancing rptr.
 */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include "m4_dcpext_afk.h"

static bool audio_probe, audio_finished;
module_param(audio_probe, bool, 0400);
MODULE_PARM_DESC(audio_probe, "Audio endpoint0x29 discovery during external hotplug; no DMA/playback");
static int audio_evidence_publish(struct dentry *directory);
static bool edid_probe, edid_finished;
static bool edid_native_ready, edid_cancel;
module_param(edid_probe, bool, 0400);
MODULE_PARM_DESC(edid_probe, "Additional AV endpoint0x28 raw EDID query during external hotplug");
static int edid_evidence_publish(struct dentry *directory);
static unsigned int afk_endpoint;
static unsigned int image_hold_ms = 1500;
module_param(image_hold_ms, uint, 0400);
static bool iboot_image;
module_param(iboot_image, bool, 0400);
static bool image_blank_cycle;
module_param(image_blank_cycle, bool, 0400);
static bool image_alternate;
module_param(image_alternate, bool, 0400);
static bool iboot_close_after_route, iboot_ready_for_route_release;
module_param(iboot_close_after_route, bool, 0400);
static bool observe_slow_quiesce;
module_param(observe_slow_quiesce, bool, 0400);
static bool quiesce_with_route, dptx_quiesce_handoff, quiesce_with_power;
module_param(quiesce_with_power, bool, 0400);
module_param(quiesce_with_route, bool, 0400);
static bool quiesce_before_afk_stop;
module_param(quiesce_before_afk_stop, bool, 0400);
MODULE_PARM_DESC(iboot_image, "Opt-in private 1080p color-bar surface; requires iboot_probe and frame_map");
static bool iboot_probe, iboot_ready_for_hpd, iboot_finished;
module_param(iboot_probe, bool, 0400);
MODULE_PARM_DESC(iboot_probe, "Additional endpoint0x23 power/mode trial while servicing DPTX; no image");
static int iboot_evidence_publish(struct dentry *directory);
module_param(afk_endpoint, uint, 0400);
MODULE_PARM_DESC(afk_endpoint, "0 for system only; AFK service endpoints 0x20/23/24/27/28/2a");
static int application_result = -ENODATA;
module_param(application_result, int, 0400);
static int afk_probe_error;
static bool afk_probe_started, afk_probe_stopping, afk_probe_stopped, afk_probe_running;
static u64 afk_probe_deadline;
static u8 *afk_probe_buffer;
static u32 afk_probe_size, afk_probe_tag, afk_probe_messages;
static u32 afk_probe_records;
static u8 afk_probe_capture[32768];
static size_t afk_probe_capture_size;
static struct dentry *afk_probe_debug;
static struct m4_dcpext_afk_ring afk_probe_rings[2]; /* TX, RX */

static ssize_t afk_probe_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset, afk_probe_capture, afk_probe_capture_size);
}

static const struct file_operations afk_probe_fops = {
	.owner = THIS_MODULE, .read = afk_probe_read, .llseek = default_llseek,
};

static ssize_t syslog_probe_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset, session_syslog.capture, session_syslog.used);
}

static const struct file_operations syslog_probe_fops = {
	.owner = THIS_MODULE, .read = syslog_probe_read, .llseek = default_llseek,
};

static ssize_t session_data_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset, file->private_data, session_data_size);
}

static const struct file_operations session_data_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = session_data_read, .llseek = default_llseek,
};

static void session_evidence_cleanup(void)
{
	debugfs_remove_recursive(afk_probe_debug);
	vfree(session_data_before);
	vfree(session_data_after);
	session_data_before = session_data_after = NULL;
	session_data_size = 0;
}

static int native_evidence_publish(struct dentry *dir);
static int session_evidence_publish(void)
{
	if (!afk_endpoint && !native_query)
		return 0;
	afk_probe_debug = debugfs_create_dir(port ? "m4_dcpext1_session" : "m4_dcpext_session", NULL);
	if (IS_ERR_OR_NULL(afk_probe_debug))
		return -ENOMEM;
	if (IS_ERR_OR_NULL(debugfs_create_file("afk-capture", 0400, afk_probe_debug, NULL, &afk_probe_fops))) {
		session_evidence_cleanup();
		return -ENOMEM;
	}
	if (IS_ERR_OR_NULL(debugfs_create_file("syslog-capture", 0400, afk_probe_debug, NULL, &syslog_probe_fops))) {
		session_evidence_cleanup();
		return -ENOMEM;
	}
	if (native_evidence_publish(afk_probe_debug) || iboot_evidence_publish(afk_probe_debug) || edid_evidence_publish(afk_probe_debug) || audio_evidence_publish(afk_probe_debug)) {
		session_evidence_cleanup();
		return -ENOMEM;
	}
	if (session_data_size) {
		if (session_data_before)
			debugfs_create_file("firmware-data-before", 0400, afk_probe_debug,
					    session_data_before, &session_data_fops);
		if (session_data_after)
			debugfs_create_file("firmware-data-after", 0400, afk_probe_debug,
					    session_data_after, &session_data_fops);
	}
	return 0;
}

static int afk_probe_send(unsigned int type, u64 payload)
{
	return session_send(NULL, afk_endpoint, (u64)type << 48 | payload);
}

#include "dcpext_session_atc.h"
#include "dcpext_afk_query.h"
#include "dcpext_iboot_adapter.h"
#include "dcpext_edid_adapter.h"
#include "dcpext_audio_adapter.h"

static int afk_probe_drain(void)
{
	u8 *header = afk_probe_rings[1].header, *data = afk_probe_rings[1].data;
	u32 size = afk_probe_rings[1].size;
	u32 block = afk_probe_rings[1].block;
	u32 rptr, wptr, bytes, total;
	u8 *qe;
	int ret;

	if (!header)
		return -EPROTO;
	for (unsigned int n = 0; n < 64; n++) {
		rptr = READ_ONCE(*(u32 *)(header + block));
		wptr = READ_ONCE(*(u32 *)(header + 2 * block));
		dma_rmb();
		if (rptr >= size || wptr >= size || (rptr | wptr) & (block - 1))
			return -EPROTO;
		if (rptr == wptr)
			return 0;
		qe = data + rptr;
		if (get_unaligned_le32(qe) != 0x20504f49)
			return -EPROTO;
		bytes = get_unaligned_le32(qe + 4);
		if (bytes > size - 16)
			return -EPROTO;
		if (bytes + 16 > size - rptr) {
			if (wptr >= rptr || !wptr)
				return -EPROTO;
			rptr = 0;
			qe = data;
			if (get_unaligned_le32(qe) != 0x20504f49)
				return -EPROTO;
			bytes = get_unaligned_le32(qe + 4);
		}
		if (bytes < 8 || bytes > size - rptr - 16)
			return -EPROTO;
		total = ALIGN(bytes + 16, block);
		if (desktop && bytes + 12 > sizeof(afk_probe_capture) - afk_probe_capture_size)
			afk_probe_capture_size = 0;
		if (total > size - rptr || (rptr < wptr && total > wptr - rptr) ||
		    bytes + 12 > sizeof(afk_probe_capture) - afk_probe_capture_size)
			return -EOVERFLOW;
		/* Capture {channel, type, epic byte length, epic bytes}. */
		put_unaligned_le32(get_unaligned_le32(qe + 8), afk_probe_capture + afk_probe_capture_size);
		put_unaligned_le32(get_unaligned_le32(qe + 12), afk_probe_capture + afk_probe_capture_size + 4);
		put_unaligned_le32(bytes, afk_probe_capture + afk_probe_capture_size + 8);
		memcpy(afk_probe_capture + afk_probe_capture_size + 12, qe + 16, bytes);
		afk_probe_capture_size += bytes + 12;
		afk_probe_records++;
		/* Always retain payload opaquely. A V2 transport sequence of 2
		 * coincides with old EPIC's version byte; guessing by that byte
		 * misclassified the third publication in the captured EP24 run.
		 */
		pr_info("m4_dcpext_session: raw AFK record channel=%u type=%u bytes=%u\n",
			get_unaligned_le32(qe + 8), get_unaligned_le32(qe + 12), bytes);
		ret = afk_query_receive(qe + 16, bytes);
		rptr += total;
		if (rptr == size)
			rptr = 0;
		dma_mb();
		WRITE_ONCE(*(u32 *)(header + block), rptr);
		if (ret)
			return ret;
	}
	return -E2BIG;
}

static int afk_probe_receive(u64 message)
{
	u32 type = message >> 48;
	u32 base, size, index, data_size, block;
	u64 dva;
	int ret;

	if (!(desktop && session.runtime) && ++afk_probe_messages > (link_training ? 256 : 64)) {
		pr_err("m4_dcpext_session: AFK message budget exhausted count=%u training=%u\n",
			afk_probe_messages, link_training);
		return -E2BIG;
	}
	if (afk_probe_stopped)
		return -EPROTO;
	switch (type) {
	case 0xa0:
		return 0;
	case 0x89:
		if (afk_probe_buffer || afk_probe_stopping)
			return -EPROTO;
		size = ((message >> 16) & 0xffff) * 64;
		if (!size || size > LEASE_SIZE - session_used)
			return -ENOSPC;
		afk_probe_buffer = pool + session_used;
		ret = session_buffer(NULL, afk_endpoint, 0, size, &dva);
		if (ret)
			return ret;
		afk_probe_size = size;
		afk_probe_tag = message & 0xffff;
		return afk_probe_send(0xa1, dva);
	case 0x8a:
	case 0x8b:
		index = type == 0x8a ? 0 : 1;
		base = ((message >> 32) & 0xffff) * 64;
		size = ((message >> 16) & 0xffff) * 64;
		if (!afk_probe_buffer || afk_probe_rings[index].header || afk_probe_stopping ||
		    (message & 0xffff) != afk_probe_tag || size < 256 || base >= afk_probe_size ||
		    size > afk_probe_size - base)
			return -EPROTO;
		if (afk_probe_rings[!index].header &&
		    base < afk_probe_rings[!index].offset + afk_probe_rings[!index].extent &&
		    afk_probe_rings[!index].offset < base + size)
			return -EPROTO;
		data_size = READ_ONCE(*(u32 *)(afk_probe_buffer + base));
		if (data_size >= size || (size - data_size) % 3)
			return -EPROTO;
		block = (size - data_size) / 3;
		pr_info("m4_dcpext_session: AFK ring=%u extent=%#x data=%#x block=%#x\n", index, size, data_size, block);
		if (block < 64 || block > 256 || !is_power_of_2(block) ||
		    data_size < block || data_size % block)
			return -EPROTO;
		afk_probe_rings[index].header = afk_probe_buffer + base;
		afk_probe_rings[index].data = afk_probe_buffer + base + 3 * block;
		afk_probe_rings[index].size = data_size;
		afk_probe_rings[index].block = block;
		afk_probe_rings[index].offset = base;
		afk_probe_rings[index].extent = size;
		if (afk_probe_rings[0].header && afk_probe_rings[1].header)
			return afk_probe_send(0xa3, 0);
		return 0;
	case 0x86:
		if (!afk_probe_rings[0].header || !afk_probe_rings[1].header || afk_probe_running)
			return -EPROTO;
		afk_probe_running = true;
		return 0;
	case 0x85:
		return afk_probe_drain();
	case 0x8c:
		/* The existing m1n1 AFK client treats this message as a no-op. */
		return 0;
	case 0xc1:
		if (!afk_probe_stopping)
			return -EPROTO;
		afk_probe_stopped = true;
		if (afk_query)
			pr_info("m4_dcpext_session: query_result=%d close_report_sent=%u peer_close_report=%u TX_consumed=%u AFK_shutdown_ACK=1\n",
				afk_query_result, afk_query_closing, afk_query_closed, afk_query_tx_empty());
		application_result = afk_probe_error ? afk_probe_error : afk_query ?
			(afk_query_closing && afk_query_tx_empty() ? afk_query_result : -ETIMEDOUT) : afk_probe_records ? 0 : -ENODATA;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int afk_probe_fail(int ret)
{
	afk_probe_error = ret;
	application_result = ret;
	if (afk_probe_stopping)
		return 0; /* Keep waiting for the already-requested shutdown ACK. */
	afk_probe_stopping = true;
	pr_info("m4_dcpext_session: AFK error=%d; requesting acknowledged shutdown with buffers retained\n", ret);
	return afk_probe_send(0xc0, 0);
}

static int afk_probe_handle(u64 message)
{
	int ret;
	/* A failed parser stops interpreting data while all buffers remain mapped. */
	if (afk_probe_error && (message >> 48) != 0xc1)
		return 0;
	ret = afk_probe_receive(message);
	return !ret || afk_probe_stopped ? ret : afk_probe_fail(ret);
}

static int afk_probe_progress(void)
{
	int ret;
	if (!afk_endpoint)
		return 0;
	if (afk_endpoint != 0x20 && afk_endpoint != 0x23 && afk_endpoint != 0x24 &&
	    afk_endpoint != 0x27 && afk_endpoint != 0x28 && afk_endpoint != 0x2a)
		return -EINVAL;
	ret = audio_adapter_progress();
	if (ret)
		return ret;
	ret = edid_adapter_progress();
	if (ret)
		return ret;
	ret = iboot_adapter_progress();
	if (ret)
		return ret;
	if (!afk_probe_started) {
		if (!(session.endpoints[afk_endpoint / 32] & BIT(afk_endpoint % 32)))
			return -ENODEV;
		afk_probe_started = true;
		afk_probe_deadline = session_now(NULL) + (native_kms_trial ? 14000 : iboot_image && image_hold_ms > 1500 ? image_hold_ms + 3500 : afk_query >= 7 ? 5000 : afk_query >= 5 ? 4000 : 2000);
		ret = session_send(NULL, 0, (5ULL << 52) | ((u64)afk_endpoint << 32) | 2);
		return ret ? ret : afk_probe_send(0x80, 0);
	}
	if (afk_query && !afk_probe_stopping) {
		ret = afk_query_progress();
		if (ret)
			return afk_probe_fail(ret);
	}
	if (quiesce_before_afk_stop && !session.ap_quiesced && dptx_trial_phase == DPTX_TRIAL_DONE)
		return 0;
	if (!afk_probe_stopping && ((afk_query ? afk_query_closed : afk_probe_records) ||
	    (!session.runtime && session_now(NULL) >= afk_probe_deadline))) {
		afk_probe_stopping = true;
		return afk_probe_send(0xc0, 0);
	}
	return 0;
}
