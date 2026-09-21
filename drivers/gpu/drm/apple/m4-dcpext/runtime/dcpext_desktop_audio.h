/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* ALSA-facing transport, using Asahi's service-call framing with the M4
 * AFKEP v2 envelope. All FIFO access shares the display route mutex.
 */
static u64 native_audio_generation(void)
{
	return ((u64)desktop_hpd_disconnects << 32) | (audio_generation + 1);
}

static int native_audio_identify(char *name, size_t size, u64 *generation)
{
	guard(mutex)(&native_route_lock);
	if (!native_kms_ready || native_kms_finished || !native_kms_connected ||
	    !session.runtime || session.phase != DCPEXT_RUNNING)
		return -ENODEV;
	strscpy(name, dptx_monitor_name, size);
	*generation = native_audio_generation();
	return 0;
}

static int native_audio_call(u16 group, u32 command, void *data, u32 bytes,
			     u64 *generation)
{
	u64 deadline;
	u8 *p = audio_state.packet;
	u32 size = 96 + bytes;
	int ret;

	if (bytes > AUDIO_CAP + 48 || !data || !generation)
		return -EINVAL;
	guard(mutex)(&native_route_lock);
	if (!native_kms_ready || native_kms_finished || !native_kms_connected ||
	    !session.runtime || session.phase != DCPEXT_RUNNING)
		return -ENODEV;
	if (*generation && *generation != native_audio_generation())
		return -ENODEV;
	if (audio_channel.error || audio_rpc_pending)
		return -EIO;
	audio_desktop = true;
	deadline = session_now(NULL) + 2000;
	while (!audio_state.opened) {
		ret = audio_adapter_progress();
		if (ret)
			goto fault;
		if (audio_state.opened)
			break;
		ret = native_poll(NULL, msecs_to_jiffies(5));
		if (ret && ret != -ETIMEDOUT)
			goto fault;
		if (session_now(NULL) >= deadline) {
			ret = -ETIMEDOUT;
			goto fault;
		}
	}
	*generation = native_audio_generation();
	memset(p, 0, size);
	p[0] = audio_state.sequence++;
	put_unaligned_le16(audio_state.interface, p + 2);
	put_unaligned_le32(size - 8, p + 4);
	p[16] = 0xc0;
	p[17] = 1;
	p[25] = ++audio_state.tag;
	put_unaligned_le32(size - 32, p + 28);
	put_unaligned_le16(group, p + 34);
	put_unaligned_le32(command, p + 36);
	put_unaligned_le32(bytes, p + 40);
	put_unaligned_le32(0x69706378, p + 44);
	memcpy(p + 96, data, bytes);
	audio_rpc_pending = true;
	audio_rpc_error = 0;
	ret = audio_trial_write(NULL, p, size);
	if (ret)
		goto fault;
	deadline = session_now(NULL) + 2000;
	while (audio_rpc_pending) {
		ret = native_poll(NULL, msecs_to_jiffies(5));
		if (ret && ret != -ETIMEDOUT)
			goto fault;
		if (session_now(NULL) >= deadline) {
			ret = -ETIMEDOUT;
			goto fault;
		}
	}
	ret = audio_rpc_error;
	if (ret)
		goto fault;
	if (*generation != native_audio_generation())
		return -ENODEV;
	if (audio_rpc_bytes < 64 ||
	    get_unaligned_le32(audio_rx) != (u32)group << 16 ||
	    get_unaligned_le32(audio_rx + 4) != command ||
	    get_unaligned_le32(audio_rx + 12) != 0x69706378 ||
	    get_unaligned_le32(audio_rx + 8) > bytes ||
	    get_unaligned_le32(audio_rx + 8) > audio_rpc_bytes - 64) {
		ret = -EPROTO;
		goto fault;
	}
	memset(data, 0, bytes);
	memcpy(data, audio_rx + 64, get_unaligned_le32(audio_rx + 8));
	audio_rx_bytes = audio_rpc_bytes;
	audio_result = 0;
	pr_info("audio service group=%u command=%u completed bytes=%u\n",
		group, command, audio_rpc_bytes);
	return 0;
fault:
	/* Never reuse an OOB buffer after an uncertain reply. Audio faults must
	 * not stop video or free memory still addressable by DCP.
	 */
	audio_result = audio_channel.error = ret;
	audio_rpc_pending = false;
	pr_err("audio service group=%u command=%u failed %d; DMA retained\n",
		group, command, ret);
	return ret;
}
static const struct m4_dcpext_audio_ops native_audio_ops = {
	.call = native_audio_call,
	.identify = native_audio_identify,
};
