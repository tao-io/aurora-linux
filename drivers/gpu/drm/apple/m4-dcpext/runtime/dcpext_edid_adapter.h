/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* DCP AV-service EDID query while iBoot/DPTX maintain the external link.
 * Endpoint/command: Janne Grunau / Asahi dpavservep.c, commit666b23ae0b3e.
 * Service identity verified in edid-discovery-20260914-1 on J713.
 */
#include "dcpext_edid_trial.h"
static struct afkc edid_channel;
static struct edid_trial edid_state;
static u16 edid_retired_interface;
static u64 edid_stop_at_ms;
static int edid_result = -ENODATA;
module_param(edid_result, int, 0400);

static int edid_trial_write(void *cookie, const u8 *p, u32 bytes)
{
	pr_info("m4_dcpext_session: EDID TX interface=%u type=%#x bytes=%u\n",
		edid_state.interface, p[16], bytes);
	return afkc_write(&edid_channel, p, bytes);
}

static int edid_adapter_allocate(void *cookie, u32 size, u8 **buffer, u64 *dva)
{
	size_t offset = session_used;
	int ret = session_buffer(NULL, 0x28, 0, size, dva);
	if (!ret)
		*buffer = pool + offset;
	return ret;
}

static int edid_adapter_record(void *cookie, const u8 *p, u32 bytes)
{
	int ret;
	u16 interface;
	if (bytes < 24 || get_unaligned_le32(p + 4) != bytes - 8)
		return -EPROTO;
	interface = get_unaligned_le16(p + 2);
	pr_info("m4_dcpext_session: EDID RX interface=%u type=%#x category=%u bytes=%u\n",
		interface, p[16], p[17], bytes);
	if (p[16] == 0x11 && !p[17]) {
		ret = afk_query_publication_named(p, bytes, interface,
			"dispext0:dcpav-service-epic:0");
		if (ret <= 0)
			return ret;
		if (!interface || edid_state.interface)
			return -EPROTO;
		edid_state.interface = interface;
		return 0;
	}
	/* AFKEPInterfaceKextV2::handleUnknown maps report0x14 to
	 * enqueueTerminateEvent(interface). HDMI HPD-low retires the old
	 * AV service; HPD-high publishes a new interface on the same channel.
	 * Keep transport sequence/tag counters, invalidate all sink data, and
	 * reopen the newly published service before requesting its EDID.
	 */
	if (p[16] == 0x14 && !p[17] && interface == edid_state.interface) {
		if (bytes != 25 || p[24])
			return -EPROTO;
		edid_retired_interface = interface;
		edid_state.interface = 0;
		edid_state.opened = edid_state.pending = edid_state.received = false;
		edid_state.close_sent = false;
		edid_state.bytes = 0;
		edid_state.error = 0;
		edid_stop_at_ms = 0;
		edid_result = -ENODATA;
		return 0;
	}
	/* A late inline reply belongs to the retired service, never the new
	 * sink. No DMA buffer is freed or reused on service retirement. */
	if (edid_retired_interface && interface == edid_retired_interface &&
	    interface != edid_state.interface)
		return 0;
	/* This channel also carries reports for unselected AV services. */
	if (!p[17] && interface != edid_state.interface)
		return 0;
	if (!edid_state.interface || interface != edid_state.interface)
		return -EPROTO;
	if (!p[17])
		return 0; /* Reports retained; no report proves EDID success. */
	ret = edid_trial_response(&edid_state, p, bytes);
	if (ret)
		edid_state.error = edid_result = ret;
	else
		pr_info("m4_dcpext_session: EDID validated %u bytes, %u checksummed blocks\n",
			edid_state.bytes, edid_state.bytes / 128);
	return ret;
}

static int edid_adapter_receive(u64 message)
{
	int ret = afkc_receive(&edid_channel, message);
	if (ret)
		edid_result = ret;
	return ret;
}

static int edid_adapter_progress(void)
{
	static const struct afkc_ops ops = {
		.send = session_send, .allocate = edid_adapter_allocate,
		.record = edid_adapter_record,
	};
	if (!edid_probe)
		return 0;
	if (!edid_channel.started) {
		if (edid_cancel) {
			edid_finished = true;
			return 0;
		}
		if (!(session.endpoints[1] & BIT(8)))
			return -ENODEV;
		edid_channel.max_payload = EDID_PACKET;
		return afkc_start(&edid_channel, &ops, NULL, 0x28);
	}
	if (edid_channel.error)
		return edid_channel.error;
	if (edid_channel.stopped) {
		edid_finished = true;
		edid_result = edid_state.received ? 0 : -ENODATA;
		return 0;
	}
	if (!edid_channel.running || edid_channel.stopping)
		return 0;
	if (!edid_cancel && edid_state.interface && !edid_state.opened) {
		edid_state.opened = true;
		return edid_trial_send(&edid_state, NULL, false, 0x12);
	}
	/* Read only once the populated timing/color lists qualify active HPD.
	 * Private inline capacity bounds both directions; no received DMA address.
	 */
	if (!edid_cancel && edid_state.opened && (edid_native_ready || iboot_state.phase == M4_DCPEXT_IBOOT_POWER_OFF) &&
	    !edid_state.pending && !edid_state.received)
		return edid_trial_send(&edid_state, NULL, true, 0);
	/* Sink unplug is not endpoint shutdown. Keep its sequence/tag state so
	 * an outstanding old query can drain before the next connection query. */
	if (desktop && session.runtime)
		return 0;
	/* Native client keeps the AV service available throughout link teardown.
	 * Closing it immediately after the read was unsafe in the iBoot trial. */
	if ((edid_native_ready || edid_cancel) && (afk_hpd_dirty || afk_display_dirty || afk_route_dirty))
		return 0;
	/* Cancellation is not an EDID success. Wait for any outstanding reply,
	 * pair an actual service open with close, and require channel stop ACK. */
	if (edid_cancel && !edid_state.opened)
		return afkc_stop(&edid_channel);
	if ((edid_state.received || edid_cancel) && !edid_state.pending && !edid_state.close_sent) {
		edid_state.close_sent = true;
		edid_stop_at_ms = session_now(NULL) + 100;
		return edid_trial_send(&edid_state, NULL, false, 0x13);
	}
	if (edid_stop_at_ms && session_now(NULL) >= edid_stop_at_ms && afkc_tx_empty(&edid_channel))
		return afkc_stop(&edid_channel);
	return 0;
}

static ssize_t edid_probe_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset,
		edid_channel.capture, edid_channel.capture_size);
}
static const struct file_operations edid_probe_fops = {
	.owner = THIS_MODULE, .read = edid_probe_read, .llseek = default_llseek,
};
static ssize_t edid_raw_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset, edid_state.raw, edid_state.bytes);
}
static const struct file_operations edid_raw_fops = {
	.owner = THIS_MODULE, .read = edid_raw_read, .llseek = default_llseek,
};
static int edid_evidence_publish(struct dentry *directory)
{
	if (!edid_probe)
		return 0;
	if (IS_ERR_OR_NULL(debugfs_create_file("edid-service-capture", 0400, directory,
		NULL, &edid_probe_fops)))
		return -ENOMEM;
	return IS_ERR_OR_NULL(debugfs_create_file("edid-raw", 0400, directory,
		NULL, &edid_raw_fops)) ? -ENOMEM : 0;
}
