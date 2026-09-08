/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_SERVICE_H
#define G16_RENDER_SERVICE_H

/* Bounded lifetime test for a future DRM submission backend. Userspace owns
 * the pacing, but this first gate accepts only the eight qualified RGB draws.
 * It exposes neither command pointers nor arbitrary GPU memory to userspace.
 */
static struct task_struct *g16_render_thread;
static struct dentry *g16_render_service_file;
static DEFINE_MUTEX(g16_render_service_mutex);
static DEFINE_SPINLOCK(g16_render_service_state_lock);
static DECLARE_COMPLETION(g16_render_service_done);
static bool g16_render_service_ready;
static bool g16_render_service_stopped;
static bool g16_render_service_stop_requested;
static u32 g16_render_service_pending;
static u32 g16_render_service_completed;
static int g16_render_service_result = -EINPROGRESS;

static ssize_t g16_render_service_read(struct file *file, char __user *buf,
				       size_t count, loff_t *pos)
{
	char status[128];
	int size;

	size = scnprintf(status, sizeof(status), "ready=%u pending=%u completed=%u result=%d stopped=%u\n",
		READ_ONCE(g16_render_service_ready), READ_ONCE(g16_render_service_pending),
		smp_load_acquire(&g16_render_service_completed), READ_ONCE(g16_render_service_result),
		smp_load_acquire(&g16_render_service_stopped));
	return simple_read_from_buffer(buf, count, pos, status, size);
}

static ssize_t g16_render_service_write(struct file *file, const char __user *buf,
					size_t count, loff_t *pos)
{
	u32 sequence;
	long waited;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (render_drm_submit) {
		char command[5];

		if (count != sizeof(command))
			return -EINVAL;
		if (copy_from_user(command, buf, sizeof(command)))
			return -EFAULT;
		if (memcmp(command, "stop\n", sizeof(command)))
			return -EINVAL;
		mutex_lock(&g16_render_service_mutex);
		spin_lock(&g16_render_service_state_lock);
		WRITE_ONCE(g16_render_service_ready, false);
		WRITE_ONCE(g16_render_service_stop_requested, true);
		spin_unlock(&g16_render_service_state_lock);
		mutex_unlock(&g16_render_service_mutex);
		return count;
	}
	if (!count || count > 16)
		return -EINVAL;
	ret = kstrtou32_from_user(buf, count, 10, &sequence);
	if (ret)
		return ret;
	if (!mutex_trylock(&g16_render_service_mutex))
		return -EBUSY;
	if (!READ_ONCE(g16_render_service_ready)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (READ_ONCE(g16_render_service_pending)) {
		ret = -EBUSY;
		goto out;
	}
	if (sequence != smp_load_acquire(&g16_render_service_completed) + 1 ||
	    sequence > render_frames) {
		ret = -EINVAL;
		goto out;
	}
	reinit_completion(&g16_render_service_done);
	smp_store_release(&g16_render_service_pending, sequence);
	waited = wait_for_completion_killable_timeout(&g16_render_service_done,
						     msecs_to_jiffies(3000));
	ret = waited < 0 ? waited : !waited ? -ETIMEDOUT : READ_ONCE(g16_render_service_result);
out:
	mutex_unlock(&g16_render_service_mutex);
	return ret ? ret : count;
}

static const struct file_operations g16_render_service_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = g16_render_service_read,
	.write = g16_render_service_write,
};

static void g16_render_service_finish(int result)
{
	spin_lock(&g16_render_service_state_lock);
	WRITE_ONCE(g16_render_service_ready, false);
	WRITE_ONCE(g16_render_service_result, result);
	spin_unlock(&g16_render_service_state_lock);
	complete_all(&g16_render_service_done);
}

static int g16_render_service_run(void __iomem *asc, u64 crashlog_iova,
				  unsigned int crashlog_pages)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(render_service_seconds * 1000);
	u64 message;
	u32 endpoint, sequence;
	int ret = 0;

	smp_store_release(&g16_render_service_ready, true);
	pr_info("g16_lab: render: userspace-paced service ready, maximum %u frames in %u seconds\n",
		render_frames, render_service_seconds);
	while (g16_render_service_completed < render_frames) {
		if (READ_ONCE(g16_render_service_stop_requested))
			break;
		if (kthread_should_stop()) {
			ret = -ECANCELED;
			break;
		}
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		sequence = smp_load_acquire(&g16_render_service_pending);
		if (sequence) {
			if (!(sequence & 127))
				g16_lab_log_gpu_dvfs_constraints("render service");
			if (g16_drm_active_job) {
				ret = g16_drm_job_enter(g16_drm_active_job);
				if (ret)
					break;
			}
			ret = g16_render_buffer_begin();
			if (ret)
				break;
			g16_render_frame = sequence - 1;
			if (g16_render_frame)
				g16_render_next_frame();
			ret = g16_render_test_ta(asc, crashlog_iova, crashlog_pages);
			if (!ret || ret == -ENODATA)
				ret = g16_render_test_fragment(asc, crashlog_iova, crashlog_pages);
			if (!ret && g16_drm_active_job)
				ret = g16_drm_job_leave(g16_drm_active_job);
			if (!ret)
				g16_render_buffer_end(0);
			WRITE_ONCE(g16_render_service_result, ret);
			if (!ret)
				smp_store_release(&g16_render_service_completed, sequence);
			WRITE_ONCE(g16_render_service_pending, 0);
			if (!ret && g16_drm_active_job)
				g16_drm_job_complete(0);
			complete_all(&g16_render_service_done);
			if (ret)
				break;
			continue;
		}
		/* Continue servicing firmware while the application is idle. */
		ret = g16_lab_drain_statistics_ring();
		if (ret || g16_firmware_recovery_observed) {
			ret = ret ? ret : -EIO;
			break;
		}
		ret = g16_asc_recv_timeout(asc, &message, &endpoint, 1000);
		if (!ret && endpoint == 1 &&
		    FIELD_GET(RTKIT_MGMT_TYPE, message) == RTKIT_BUFFER_REQUEST &&
		    FIELD_GET(RTKIT_BUFFER_IOVA, message) == crashlog_iova) {
			g16_lab_capture_crashlog(crashlog_iova, crashlog_pages);
			ret = -EIO;
			break;
		}
		if (ret == -ETIMEDOUT)
			ret = 0;
		if (ret)
			break;
		cond_resched();
	}
	g16_render_service_finish(ret);
	return ret;
}

static int g16_render_thread_fn(void *unused)
{
	int ret = g16_lab_uat_initdata_map_test();

	if (ret || READ_ONCE(stage_result) == -EINPROGRESS)
		WRITE_ONCE(stage_result, ret);
	/* Failed/cancelled work releases its fence only after firmware teardown. */
	g16_render_buffer_end(READ_ONCE(stage_result) ?: -ECANCELED);
	g16_render_service_finish(READ_ONCE(stage_result));
	g16_drm_job_complete(READ_ONCE(stage_result) ?: -ECANCELED);
	smp_store_release(&g16_render_service_stopped, true);
	return ret;
}

static int g16_render_service_start(void)
{
	int ret;

	g16_render_service_file = debugfs_create_file("g16_lab_render_submit", 0600,
		NULL, NULL, &g16_render_service_fops);
	if (IS_ERR(g16_render_service_file)) {
		ret = PTR_ERR(g16_render_service_file);
		g16_render_service_file = NULL;
		return ret;
	}
	WRITE_ONCE(stage_result, -EINPROGRESS);
	g16_render_thread = kthread_create(g16_render_thread_fn, NULL, "g16-render");
	if (IS_ERR(g16_render_thread)) {
		ret = PTR_ERR(g16_render_thread);
		g16_render_thread = NULL;
		debugfs_remove(g16_render_service_file);
		g16_render_service_file = NULL;
		return ret;
	}
	get_task_struct(g16_render_thread);
	wake_up_process(g16_render_thread);
	return 0;
}

static void g16_render_service_stop(void)
{
	debugfs_remove(g16_render_service_file);
	g16_render_service_file = NULL;
	if (g16_render_thread) {
		kthread_stop_put(g16_render_thread);
		g16_render_thread = NULL;
	}
}
#endif
