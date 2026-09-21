/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include "dcpext_native_query.h"
#include "m4_dcpext_rpc.h"
#include "m4_dcpext_native.h"
#include "m4_dcpext_surface.h"
#include "m4_dcpext_audio.h"
static const struct m4_dcpext_audio_ops native_audio_ops;
static struct dcpext_native_query native_state;
static unsigned int native_sys_error, native_pio_error;
module_param(native_sys_error, uint, 0400);
module_param(native_pio_error, uint, 0400);
static struct m4_dcpext_rpc *native_rpc;
static struct m4_dcpext_native *native_client;
static bool native_startup_attempted, native_startup_done;
static bool native_system_shutdown;
module_param(native_system_shutdown, bool, 0400);
static bool native_stop_signal;
module_param(native_stop_signal, bool, 0400);
static bool native_mode_done;
static bool native_route_cancelled;
static int native_mode_result = -ENODATA;
static int native_power_result = -ENODATA;
static int native_background_result = -ENODATA;
static int native_frame_result = -ENODATA;
module_param(native_mode_done, bool, 0400);
module_param(native_mode_result, int, 0400);
module_param(native_power_result, int, 0400);
module_param(native_background_result, int, 0400);
module_param(native_frame_result, int, 0400);
static int native_startup_result = -ENODATA;
module_param(native_startup_done, bool, 0400);
module_param(native_startup_result, int, 0400);
static struct {
	struct m4_dcpext_buffer info;
	bool retired;
} native_buffers[16];
static u32 native_buffer_count;

module_param_named(native_phase, native_state.phase, uint, 0400);
module_param_named(native_hash, native_state.hash, uint, 0400);
module_param_named(native_alignment, native_state.alignment, uint, 0400);
module_param_named(native_is_main, native_state.is_main, uint, 0400);
module_param_named(native_error, native_state.error, int, 0400);

static int native_allocate(void *cookie, u32 size, u8 **cpu, u64 *dva)
{
	size_t offset = session_used;
	int ret = session_buffer(NULL, 0x37, 0, size, dva);

	if (!ret)
		*cpu = pool + offset;
	return ret;
}
static const struct dcpext_native_query_ops native_ops = {
	.allocate = native_allocate,
	.send = session_send,
};
/* Bounded KMS capture: enough for eight seconds of real flips. */
static u8 native_capture[0x1000000];
static u32 native_capture_size;
static int native_record(void *cookie, u32 kind, u64 message, const void *data,
			 u32 size)
{
	u8 *dst = native_capture + native_capture_size;
	/* Full RPC capture is a finite qualification artifact, not a desktop
	 * dependency. Keep startup evidence; do not fail display on log capacity.
	 */
	if (desktop && session.runtime)
		return 0;
	if (native_capture_size > sizeof(native_capture) - 16 ||
	    size > sizeof(native_capture) - native_capture_size - 16)
		return desktop ? 0 : -ENOSPC;
	put_unaligned_le32(kind, dst);
	put_unaligned_le32(size, dst + 4);
	put_unaligned_le64(message, dst + 8);
	if (size)
		memcpy(dst + 16, data, size);
	native_capture_size += size + 16;
	return 0;
}
static ssize_t native_capture_read(struct file *file, char __user *buf,
				   size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset, native_capture,
				       native_capture_size);
}
static const struct file_operations native_capture_fops = {
	.owner = THIS_MODULE,
	.read = native_capture_read,
	.llseek = default_llseek,
};
static ssize_t native_crashlog_read(struct file *file, char __user *buf,
				   size_t count, loff_t *offset)
{
	return simple_read_from_buffer(buf, count, offset, session_crashlog,
				       smp_load_acquire(&session_crashlog_size));
}
static const struct file_operations native_crashlog_fops = {
	.owner = THIS_MODULE,
	.read = native_crashlog_read,
	.llseek = default_llseek,
};
static int native_evidence_publish(struct dentry *dir)
{
	if (!native_startup)
		return 0;
	if (IS_ERR_OR_NULL(debugfs_create_file("crashlog", 0400, dir,
					     NULL, &native_crashlog_fops)))
		return -ENOMEM;
	return IS_ERR_OR_NULL(debugfs_create_file("native-capture", 0400, dir,
						  NULL, &native_capture_fops)) ?
		       -ENOMEM :
		       0;
}
static int native_poll(void *cookie, unsigned long timeout)
{
	u64 deadline = session_now(NULL) + jiffies_to_msecs(timeout);
	int ret;
	do {
		u64 message, endpoint;
		ret = dcpext_session_poll(&session);
		if (ret)
			return ret;
		if (!(readl(session_fifo + 0x114) & BIT(17))) {
			message = readq(session_fifo + 0x830);
			endpoint = readq(session_fifo + 0x838);
			return session_dispatch(endpoint, message);
		}
		usleep_range(100, 200);
	} while (session_now(NULL) < deadline);
	return -ETIMEDOUT;
}
static int native_buffer_alloc(void *cookie, struct m4_dcpext_buffer *info)
{
	size_t offset = session_used;
	u64 dva;
	int ret;
	if (!info->size || info->size > 0x100000 ||
	    native_buffer_count == ARRAY_SIZE(native_buffers))
		return -E2BIG;
	ret = session_buffer(NULL, 0x37, 0, info->size, &dva);
	if (ret)
		return ret;
	info->size = ALIGN(info->size, LEASE_PAGE);
	info->dva = dva;
	info->physical = pool_dma + offset;
	info->id = ++native_buffer_count;
	native_buffers[info->id - 1].info = *info;
	dma_wmb();
	pr_info("m4_dcpext_native: allocation id=%u bytes=%#llx physical=%#llx dva=%#llx retained until quiescence\n",
		info->id, info->size, info->physical, info->dva);
	return 0;
}
static int native_buffer_map(void *cookie, struct m4_dcpext_buffer *info)
{
	if (!info->id || info->id > native_buffer_count ||
	    native_buffers[info->id - 1].retired || !pio_published ||
	    !pio_registers_match())
		return -EINVAL;
	*info = native_buffers[info->id - 1].info;
	pr_info("m4_dcpext_native: PIODMA id=%u bytes=%#llx dva=%#llx\n",
		info->id, info->size, info->dva);
	return 0;
}
static int native_buffer_retire(void *cookie, u32 id)
{
	if (!id || id > native_buffer_count || native_buffers[id - 1].retired)
		return -EINVAL;
	native_buffers[id - 1].retired = true; /* Still pinned in both DARTs. */
	return 0;
}
static int native_dart_power(void *cookie, bool on)
{
	int ret = fabric_powered();
	/* This bounded diagnostic holds a session power vote through quiescence.
  * Releasing the firmware vote never releases that independent obligation. */
	if (ret)
		return ret;
	if (!pinned || !pio_published || !pio_registers_match())
		return -EUCLEAN;
	pr_info("m4_dcpext_native: firmware DART power vote=%u, session vote retained\n",
		on);
	return 0;
}
static const struct m4_dcpext_rpc_ops native_rpc_ops = {
	.send = session_send,
	.poll = native_poll,
	.alloc = native_buffer_alloc,
	.map = native_buffer_map,
	.retire = native_buffer_retire,
	.dart_power = native_dart_power,
	.record = native_record,
};
static int native_drain(void)
{
	int ret = m4_dcpext_native_poll(native_client);
	return ret == -ETIMEDOUT || ret == -EAGAIN ? 0 : ret;
}
static int native_stop(void)
{
	int ret;

	if (!native_startup_done)
		return 0;
	ret = native_system_shutdown ?
		m4_dcpext_native_stop_for_shutdown(native_client) :
		m4_dcpext_native_stop(native_client);
	if (!ret && native_stop_signal)
		ret = m4_dcpext_native_shutdown(native_client);
	return ret;
}
static void native_cleanup(void)
{
	if (native_client && !IS_ERR(native_client))
		m4_dcpext_native_destroy(native_client);
	if (native_rpc && !IS_ERR(native_rpc))
		m4_dcpext_rpc_destroy(native_rpc);
}
#include "dcpext_kms_adapter.h"
#include "dcpext_desktop_audio.h"

static bool native_cancel_untrained_route(void)
{
	if (native_route_cancelled)
		return true;
	if (!native_mode_trial || !native_startup_done || native_mode_done ||
	    dptx_trial_phase != DPTX_TRIAL_HPD_WAIT ||
	    session_now(NULL) < dptx_trial_observe_until + 3000)
		return false;
	/* Leave time for HPD-off/release/disconnect and both power ACKs.
	 * The captured sink error sequence retries five times over ~2.5s.
	 * Do not consume the entire session deadline waiting for absent modes.
	 */
	native_route_cancelled = true;
	native_route_pending = false;
	edid_cancel = true;
	native_mode_result = dptx_errors.last_command ? -ENOLINK : -ETIMEDOUT;
	pr_warn("m4_dcpext_native: mode discovery failed %d; cancelling route with all teardown acknowledgments required\n",
		native_mode_result);
	return true;
}

static int native_progress(void)
{
	if (!native_query)
		return 0;
	if (native_cancel_untrained_route())
		return 0;
	if (native_mode_done && edid_probe)
		native_route_pending = !edid_state.received || (native_kms_trial && !native_kms_finished);
	if (native_kms_trial && native_mode_done) {
		int ret = native_kms_publish();
		if (ret)
			return ret;
	}
	/* Test native power/client shutdown while the trained route and AFK
	 * service are still available to satisfy firmware dependencies. */
	if (native_close_before_route && native_startup_done &&
	    dptx_trial_phase == DPTX_TRIAL_HPD_WAIT &&
	    session_now(NULL) >= dptx_trial_observe_until)
		return native_stop();
	if (native_mode_trial && native_startup_done && !native_mode_done) {
		struct m4_dcpext_native_mode *modes;
		u32 count, selected;
		u64 generation;
		struct m4_dcpext_native_mode wanted = {
			.geometry = { 1920, 1080, 148500, 2200, 1125, false, false },
			.hfront = 88, .hsync = 44, .vfront = 4, .vsync = 5,
			.hpositive = true, .vpositive = true,
		};
		int ret;

		modes = m4_dcpext_native_modes(native_client, &count, &generation);
		if (IS_ERR(modes))
			return PTR_ERR(modes) == -ENODATA ? 0 : PTR_ERR(modes);
		ret = m4_dcpext_native_mode_select(&wanted, modes, count, &selected);
		if (!ret) {
			pr_info("m4_dcpext_native: selecting native 1080p60 color=%u timing=%u generation=%llu\n",
				modes[selected].color_id, modes[selected].timing_id, generation);
			ret = m4_dcpext_native_set_mode(native_client, generation,
				modes[selected].color_id, modes[selected].timing_id, 7680);
		}
		kfree(modes);
		native_mode_result = ret;
		native_mode_done = !ret;
		if (!ret)
			edid_native_ready = true;
		pr_info("m4_dcpext_native: native mode trial result=%d; no surface submitted\n", ret);
		if (!ret && native_power_trial) {
			ret = m4_dcpext_native_set_power(native_client, true, generation);
			native_power_result = ret;
		}
		if (!ret && native_background_trial) {
			ret = m4_dcpext_native_swap(native_client, generation, NULL, 0, 0, 0);
			native_background_result = ret;
			pr_info("m4_dcpext_native: background swap result=%d; no framebuffer mapping\n", ret);
		}
		if (!ret && native_frame_trial) {
			u8 surface[M4_DCPEXT_SURFACE_SIZE];
			if (!frame_published || !frame_registers_match())
				return -EUCLEAN;
			ret = m4_dcpext_surface_build(surface, FRAME_WIDTH, FRAME_HEIGHT,
						     FRAME_STRIDE, false);
			dma_wmb();
			if (!ret)
				ret = m4_dcpext_native_swap(native_client, generation, surface,
							FRAME_DVA | BIT_ULL(40), FRAME_WIDTH, FRAME_HEIGHT);
			native_frame_result = ret;
			pr_info("m4_dcpext_native: framebuffer swap result=%d; allocation remains pinned\n", ret);
		}
		if (!ret)
			native_route_pending = native_kms_trial || (edid_probe && !edid_state.received);
		return ret;
	}
	if (native_startup && native_state.phase == 3 &&
	    !native_startup_attempted) {
		native_startup_attempted = true;
		if (!pio_published || !pio_registers_match())
			return -EUCLEAN;
		native_rpc = m4_dcpext_rpc_create(dma_dev, native_state.rpc,
						  &native_rpc_ops, NULL);
		if (IS_ERR(native_rpc))
			return native_startup_result = PTR_ERR(native_rpc);
		native_client =
			m4_dcpext_native_start(dma_dev, native_rpc, 7680);
		if (IS_ERR(native_client))
			return native_startup_result = PTR_ERR(native_client);
		native_startup_done = true;
		native_startup_result = 0;
		return 0;
	}
	if (native_state.phase)
		return 0;
	if (!(session.endpoints[1] & BIT(23)))
		return -ENODEV;
	return dcpext_native_query_start(&native_state, &native_ops, NULL);
}
static int native_receive(u64 message)
{
	int ret;
	if (native_rpc && !IS_ERR(native_rpc)) {
		m4_dcpext_rpc_receive(native_rpc, message);
		return 0;
	}
	ret = dcpext_native_query_receive(&native_state, &native_ops, NULL,
					  message);
	pr_info("m4_dcpext_native: RX=%#llx phase=%u alignment=%u hash=%#x main=%u ret=%d\n",
		message, native_state.phase, native_state.alignment,
		native_state.hash, native_state.is_main, ret);
	return ret;
}
