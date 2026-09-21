/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* AV audio endpoint0x29: Martin Povišer / Asahi av.c, audio.c.
 * Retrieve bounded format metadata while iBoot/DPTX hold the link.
 * No audio DMA, stream commands or playback-success claims in this stage.
 */
#include "dcpext_audio_trial.h"
#include "dcpext_afk_oob.h"
static struct audio_trial audio_state;
static bool audio_desktop, audio_rpc_pending;
static u32 audio_rpc_bytes;
static int audio_rpc_error;
static u16 audio_retired_interface;
static unsigned int audio_generation;
static u8 *audio_tx, *audio_rx;
static u64 audio_tx_dva, audio_rx_dva;
static u32 audio_rx_bytes;
static struct afkc audio_channel;
static u64 audio_stop_at_ms;
static int audio_result = -ENODATA;
module_param(audio_result, int, 0400);

static int audio_trial_write(void *cookie, const u8 *p, u32 bytes)
{
	pr_info("m4_dcpext_session: audio TX type=%#x bytes=%u\n", p[16], bytes);
	if (bytes >= 96 && p[16] == 0xc0) {
		u8 wire[AFK_OOB_PACKET];
		if (!audio_tx || !audio_rx)
			return -ENODEV;
		int ret = afk_oob_packet(wire, p, audio_tx_dva, audio_rx_dva, bytes - 32);
		if (ret)
			return ret;
		memcpy(audio_tx, p + 32, bytes - 32);
		dma_wmb();
		return afkc_write(&audio_channel, wire, sizeof(wire));
	}
	return afkc_write(&audio_channel, p, bytes);
}

static int audio_adapter_allocate(void *cookie, u32 size, u8 **buffer, u64 *dva)
{
	size_t offset = session_used;
	int ret = session_buffer(NULL, 0x29, 0, size, dva);
	if (!ret)
		*buffer = pool + offset;
	return ret;
}

static int audio_adapter_record(void *cookie, const u8 *p, u32 bytes)
{
	if (bytes < 24 || get_unaligned_le32(p + 4) != bytes - 8)
		return -EPROTO;
	pr_info("m4_dcpext_session: audio capture interface=%u type=%#x category=%u bytes=%u\n",
		get_unaligned_le16(p + 2), p[16], p[17], bytes);
	u16 interface = get_unaligned_le16(p + 2);
	if (p[16] == 0x11 && !p[17]) {
		int ret = afk_query_publication_named(p, bytes, interface,
			port ? "dispext1:dcpav-audio-interface-epic:0" : "dispext0:dcpav-audio-interface-epic:0");
		if (ret <= 0)
			return ret;
		if (!interface || audio_state.interface)
			return -EPROTO;
		audio_state.interface = interface;
		return 0;
	}
	if (p[16] == 0x14 && !p[17] && interface == audio_state.interface) {
		audio_retired_interface = interface;
		audio_state.interface = 0;
		audio_state.opened = false;
		audio_state.received = false;
		audio_state.pending = false;
		audio_generation++;
		if (audio_rpc_pending) {
			audio_rpc_error = -ENODEV;
			audio_rpc_pending = false;
		}
		return 0;
	}
	if (interface == audio_retired_interface && interface != audio_state.interface)
		return 0;
	if (!p[17] && interface != audio_state.interface)
		return 0;
	if (!audio_state.interface || interface != audio_state.interface)
		return -EPROTO;
	if (!p[17])
		return 0;
	u32 received;
	int ret = afk_oob_response(p, bytes, audio_state.interface, audio_state.tag,
		audio_rx_dva, AUDIO_PACKET - 32, &received);
	if (audio_desktop) {
		if (!audio_rpc_pending)
			return -EPROTO;
		audio_rpc_error = ret;
		if (ret)
			pr_err("audio OOB response error=%d remote_status=%#x bytes=%u\n",
				ret, get_unaligned_le32(p + 28), bytes);
		audio_rpc_bytes = ret ? 0 : received;
		audio_rpc_pending = false;
		dma_rmb();
		return 0;
	}
	if (ret || !audio_state.pending)
		return ret ? ret : -EPROTO;
	dma_rmb();
	audio_rx_bytes = received; /* Retain even an invalid service response. */
	memcpy(audio_state.packet, p, 32);
	audio_state.packet[24] = 0;
	put_unaligned_le32(received + 24, audio_state.packet + 4);
	memcpy(audio_state.packet + 32, audio_rx, received);
	return audio_trial_response(&audio_state, audio_state.packet, received + 32);
}

static int audio_adapter_receive(u64 message)
{
	int ret = afkc_receive(&audio_channel, message);
	if (ret)
		audio_result = ret;
	return audio_desktop ? 0 : ret;
}

static int audio_adapter_progress(void)
{
	static const struct afkc_ops ops = {
		.send = session_send, .allocate = audio_adapter_allocate,
		.record = audio_adapter_record,
	};
	if (!audio_probe && !audio_desktop)
		return 0;
	if (!audio_channel.started) {
		if (!(session.endpoints[1] & BIT(9)))
			return -ENODEV;
		size_t offset = session_used;
		int ret = session_buffer(NULL, 0x29, 0, AUDIO_PACKET - 32, &audio_tx_dva);
		if (ret)
			return ret;
		audio_tx = pool + offset;
		offset = session_used;
		ret = session_buffer(NULL, 0x29, 0, AUDIO_PACKET - 32, &audio_rx_dva);
		if (ret)
			return ret;
		audio_rx = pool + offset;
		audio_channel.streaming = audio_desktop;
		return afkc_start(&audio_channel, &ops, NULL, 0x29);
	}
	if (audio_channel.error)
		return audio_desktop ? 0 : audio_channel.error;
	if (audio_channel.stopped) {
		audio_finished = true;
		audio_result = audio_state.received ? 0 : -ENODATA;
		return 0;
	}
	if (!audio_channel.running || audio_channel.stopping)
		return 0;
	if (audio_state.interface && !audio_state.opened) {
		audio_state.opened = true;
		return audio_trial_send(&audio_state, NULL, false, 0x12);
	}
	if (audio_desktop)
		return 0; /* Synchronous ALSA calls own request/reply progression. */
	if (audio_state.opened && iboot_state.phase == M4_DCPEXT_IBOOT_POWER_OFF &&
	    !audio_state.pending && !audio_state.received)
		return audio_trial_send(&audio_state, NULL, true, 0);
	if (audio_state.received && !audio_state.close_sent) {
		audio_state.close_sent = true;
		audio_stop_at_ms = session_now(NULL) + 100;
		return audio_trial_send(&audio_state, NULL, false, 0x13);
	}
	if (audio_stop_at_ms && session_now(NULL) >= audio_stop_at_ms && afkc_tx_empty(&audio_channel))
		return afkc_stop(&audio_channel);
	return 0;
}

static ssize_t audio_probe_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset,
		audio_channel.capture, audio_channel.capture_size);
}
static const struct file_operations audio_probe_fops = {
	.owner = THIS_MODULE, .read = audio_probe_read, .llseek = default_llseek,
};
static ssize_t audio_oob_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset, audio_rx, audio_rx_bytes);
}
static const struct file_operations audio_oob_fops = {
	.owner = THIS_MODULE, .read = audio_oob_read, .llseek = default_llseek,
};
static int audio_evidence_publish(struct dentry *directory)
{
	if (IS_ERR_OR_NULL(debugfs_create_file("audio-service-capture", 0400, directory,
		NULL, &audio_probe_fops)))
		return -ENOMEM;
	return IS_ERR_OR_NULL(debugfs_create_file("audio-oob-response", 0400, directory,
		NULL, &audio_oob_fops)) ? -ENOMEM : 0;
}
