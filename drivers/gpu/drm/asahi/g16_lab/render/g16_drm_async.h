/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_ASYNC_H
#define G16_DRM_ASYNC_H

#include <linux/semaphore.h>

/* Queue capacity supplies backpressure, never an end-of-session cutoff. */
static struct workqueue_struct *g16_drm_submit_wq;
static struct semaphore g16_drm_queue_slots;
static DEFINE_MUTEX(g16_drm_enqueue_mutex);

static void g16_drm_job_free(struct g16_drm_job *job)
{
	u32 i;

	for (i = 0; i < job->dependency_count; i++)
		dma_fence_put(job->dependencies[i]);
	kfree(job->dependencies);
	g16_render_uat_free(&job->uat);
	kvfree(job->cache_ranges);
	if (job->owns_vm)
		g16_drm_vm_free(job->vm);
	dma_fence_put(job->fence);
	kfree(job);
}

/* Takes ownership of the supplied reference, including on failure. */
static int g16_drm_job_add_dependency(struct g16_drm_job *job, struct dma_fence *fence)
{
	struct dma_fence **next;
	u32 i, capacity;

	if (!fence)
		return 0;
	for (i = 0; i < job->dependency_count; i++) {
		if (job->dependencies[i] == fence) {
			dma_fence_put(fence);
			return 0;
		}
	}
	if (job->dependency_count == job->dependency_capacity) {
		capacity = job->dependency_capacity ? job->dependency_capacity * 2 : 16;
		if (capacity < job->dependency_capacity) {
			dma_fence_put(fence);
			return -EOVERFLOW;
		}
		next = krealloc_array(job->dependencies, capacity, sizeof(*next), GFP_KERNEL);
		if (!next) {
			dma_fence_put(fence);
			return -ENOMEM;
		}
		job->dependencies = next;
		job->dependency_capacity = capacity;
	}
	job->dependencies[job->dependency_count++] = fence;
	return 0;
}

static int g16_drm_job_async_reservations(struct g16_drm_job *job)
{
	struct g16_drm_mapping *m, *prior;
	int ret;

	/* Enqueue serialization orders this driver's reservation publication.
	 * Lock each object while collecting its old fences and attaching ours.
	 * Aliases must not attach/wait on the same job twice. */
	list_for_each_entry(m, &job->vm->mappings, link) {
		struct dma_fence **fences;
		unsigned int count = 0, i;
		bool duplicate = false;

		list_for_each_entry(prior, &job->vm->mappings, link) {
			if (prior == m)
				break;
			if (prior->obj == m->obj) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		ret = dma_resv_lock_interruptible(m->obj->resv, NULL);
		if (ret)
			return ret;
		ret = dma_resv_get_fences(m->obj->resv, DMA_RESV_USAGE_READ, &count, &fences);
		if (!ret) {
			for (i = 0; i < count; i++) {
				if (!ret)
					ret = g16_drm_job_add_dependency(job, fences[i]);
				else
					dma_fence_put(fences[i]);
			}
			kfree(fences);
		}
		if (!ret)
			ret = dma_resv_reserve_fences(m->obj->resv, 1);
		if (!ret)
			dma_resv_add_fence(m->obj->resv, job->fence, DMA_RESV_USAGE_WRITE);
		dma_resv_unlock(m->obj->resv);
		if (ret)
			return ret;
	}
	return 0;
}

static int g16_drm_job_wait_dependencies(struct g16_drm_job *job)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	u32 i;

	for (i = 0; i < job->dependency_count; i++) {
		struct dma_fence *fence = job->dependencies[i];
		long waited, remaining;

		while (!dma_fence_is_signaled(fence)) {
			if (!smp_load_acquire(&g16_render_service_ready) ||
			    READ_ONCE(g16_render_service_stop_requested))
				return -ESHUTDOWN;
			remaining = deadline - jiffies;
			if (remaining <= 0)
				return -ETIMEDOUT;
			waited = dma_fence_wait_timeout(fence, false,
				min(remaining, (long)msecs_to_jiffies(100)));
			if (waited < 0)
				return waited;
		}
		if (dma_fence_get_status(fence) < 0)
			return dma_fence_get_status(fence);
	}
	return 0;
}

static void g16_drm_submit_work(struct work_struct *work)
{
	struct g16_drm_job *job = container_of(work, struct g16_drm_job, work);
	u64 sequence;
	int ret;

	ret = g16_drm_job_wait_dependencies(job);
	if (ret)
		goto finish;
	mutex_lock(&g16_render_service_mutex);
	spin_lock(&g16_render_service_state_lock);
	if (!g16_render_service_ready || g16_render_service_stop_requested ||
	    g16_render_service_pending) {
		ret = -ESHUTDOWN;
		spin_unlock(&g16_render_service_state_lock);
		goto unlock;
	}
	sequence = g16_render_service_completed + 1;
	g16_drm_active_job = job;
	smp_store_release(&g16_render_service_pending, sequence);
	spin_unlock(&g16_render_service_state_lock);
	wake_up(&g16_render_service_work);
	/* Hardware waits are bounded by the GPU worker. On failure, done is
	 * delivered only after firmware teardown has made these pins safe. */
	wait_for_completion(&job->done);
	ret = job->result;
unlock:
	mutex_unlock(&g16_render_service_mutex);
finish:
	if (!dma_fence_is_signaled(job->fence)) {
		dma_fence_set_error(job->fence, ret ?: -ECANCELED);
		dma_fence_signal(job->fence);
	}
	if (ret)
		pr_err("g16_lab: asynchronous job failed: %d\n", ret);
	g16_drm_job_free(job);
	atomic_dec(&g16_drm_queued_jobs);
	up(&g16_drm_queue_slots);
}
#endif
