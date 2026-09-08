// SPDX-License-Identifier: GPL-2.0-only
/*
 * HACK - REVERT LATER: split J713's CPU scanout conversion over four threads.
 * This still copies into the inherited framebuffer. Remove when native DCP
 * scanout can present GPU buffers. Do not treat this as native display support.
 */

#include <linux/export.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>

#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "drm_sysfb_helper.h"

#define SYSFB_BLIT_WORKERS 4

static bool parallel_blit = true;
module_param(parallel_blit, bool, 0644);
MODULE_PARM_DESC(parallel_blit, "HACK - REVERT LATER: four-thread J713 scanout copy");

struct sysfb_blit_job {
	struct kthread_worker *worker;
	struct kthread_work work;
	struct drm_format_conv_state state;
	struct iosys_map dst;
	const struct iosys_map *src;
	const struct drm_framebuffer *fb;
	struct drm_rect clip;
	unsigned int pitch;
};

struct drm_sysfb_parallel {
	struct mutex lock;
	struct sysfb_blit_job jobs[SYSFB_BLIT_WORKERS];
};

static void sysfb_blit_work(struct kthread_work *work)
{
	struct sysfb_blit_job *job = container_of(work, struct sysfb_blit_job, work);

	drm_fb_xrgb8888_to_xrgb2101010(&job->dst, &job->pitch, job->src,
				      job->fb, &job->clip, &job->state);
	/* Drain this CPU's WC stores before the caller completes the commit. */
	wmb();
}

static void sysfb_parallel_destroy(struct drm_device *dev, void *data)
{
	struct drm_sysfb_parallel *parallel = data;
	unsigned int i;

	for (i = 0; i < SYSFB_BLIT_WORKERS; i++) {
		struct sysfb_blit_job *job = &parallel->jobs[i];

		if (job->worker)
			kthread_destroy_worker(job->worker);
		drm_format_conv_state_release(&job->state);
	}
	mutex_destroy(&parallel->lock);
}

void drm_sysfb_parallel_init(struct drm_sysfb_device *sysfb)
{
	struct drm_device *dev = &sysfb->dev;
	struct drm_sysfb_parallel *parallel;
	unsigned int i;
	size_t scratch;

	if (!of_machine_is_compatible("apple,j713") ||
	    !of_machine_is_compatible("apple,t8132") ||
	    sysfb->fb_format->format != DRM_FORMAT_XRGB2101010)
		return;

	parallel = drmm_kzalloc(dev, sizeof(*parallel), GFP_KERNEL);
	if (!parallel)
		return;
	mutex_init(&parallel->lock);

	/* Separate cached source and destination rows for each conversion helper. */
	scratch = ALIGN((size_t)sysfb->fb_pitch, ARCH_KMALLOC_MINALIGN) + sysfb->fb_pitch;
	for (i = 0; i < SYSFB_BLIT_WORKERS; i++) {
		struct sysfb_blit_job *job = &parallel->jobs[i];

		if (!drm_format_conv_state_reserve(&job->state, scratch, GFP_KERNEL))
			goto fail;
		kthread_init_work(&job->work, sysfb_blit_work);
		job->worker = kthread_run_worker(0, "sysfb-blit/%u", i);
		if (IS_ERR(job->worker)) {
			job->worker = NULL;
			goto fail;
		}
	}
	if (drmm_add_action_or_reset(dev, sysfb_parallel_destroy, parallel))
		return;
	sysfb->parallel = parallel;
	drm_info(dev, "HACK - REVERT LATER: four scanout copy workers available\n");
	return;

fail:
	sysfb_parallel_destroy(dev, parallel);
	drm_warn(dev, "Parallel scanout setup failed; using serial conversion\n");
}
EXPORT_SYMBOL_GPL(drm_sysfb_parallel_init);

bool drm_sysfb_parallel_blit(struct drm_sysfb_device *sysfb,
			     struct iosys_map dst, const struct iosys_map *src,
			     const struct drm_framebuffer *fb,
			     const struct drm_rect *clip)
{
	struct drm_sysfb_parallel *parallel = sysfb->parallel;
	unsigned int height = drm_rect_height(clip);
	unsigned int i;

	if (!parallel || !READ_ONCE(parallel_blit) ||
	    fb->format->format != DRM_FORMAT_XRGB8888 ||
	    height < SYSFB_BLIT_WORKERS ||
	    (u64)height * drm_rect_width(clip) < 65536)
		return false;

	/*
	 * Small damage stays serial. Large clips get four disjoint row bands.
	 * Complete ALL workers while the framebuffer mapping and CPU-access scope
	 * are still held. No worker may outlive atomic_update or see shared scratch.
	 */
	mutex_lock(&parallel->lock);
	for (i = 0; i < SYSFB_BLIT_WORKERS; i++) {
		struct sysfb_blit_job *job = &parallel->jobs[i];
		unsigned int first = height * i / SYSFB_BLIT_WORKERS;
		unsigned int last = height * (i + 1) / SYSFB_BLIT_WORKERS;

		job->dst = dst;
		iosys_map_incr(&job->dst, (size_t)first * sysfb->fb_pitch);
		job->src = src;
		job->fb = fb;
		job->pitch = sysfb->fb_pitch;
		job->clip = *clip;
		job->clip.y1 += first;
		job->clip.y2 = clip->y1 + last;
		kthread_queue_work(job->worker, &job->work);
	}
	for (i = 0; i < SYSFB_BLIT_WORKERS; i++)
		kthread_flush_work(&parallel->jobs[i].work);
	mutex_unlock(&parallel->lock);

	return true;
}
