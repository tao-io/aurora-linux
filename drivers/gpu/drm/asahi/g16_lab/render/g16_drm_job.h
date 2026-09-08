/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_JOB_H
#define G16_DRM_JOB_H

#include <uapi/drm/asahi_drm.h>
#include <linux/workqueue.h>
#include "g16_cache_ranges.h"

struct g16_drm_vm;

/* Queued jobs own immutable binding snapshots and independent GEM/vmap pins.
 * Completion releases these only after retirement or firmware teardown.
 * The serial diagnostic path instead holds the submitting file's VM lock.
 */
struct g16_drm_job {
	struct drm_asahi_cmd_render render;
	/* Aurora's appended EOT, partial-EOT, BG, partial-BG high dwords. */
	u32 rsrc_hi[4];
	struct g16_drm_vm *vm;
	struct g16_render_uat uat;
	struct g16_cache_range *cache_ranges;
	u32 cache_range_count;
	u64 cache_bytes;
	struct dma_fence *fence;
	struct completion done;
	int result;
	bool published;
	bool owns_vm;
	struct work_struct work;
	struct dma_fence **dependencies;
	u32 dependency_count, dependency_capacity;
};

static struct g16_drm_job *g16_drm_active_job;
static atomic_t g16_drm_queued_jobs = ATOMIC_INIT(0);
static atomic64_t g16_drm_queued_total = ATOMIC64_INIT(0);
static int g16_drm_job_enter(struct g16_drm_job *job);
static void g16_drm_job_log_fault(struct g16_drm_job *job);
static int g16_drm_job_leave(struct g16_drm_job *job);
static void g16_drm_job_complete(int result);

static u64 g16_render_vdm_base(void)
{
	if (g16_drm_active_job)
		return g16_drm_active_job->render.vdm_ctrl_stream_base - 0x1000000000ULL;
	return render_stream ? 0x1c000 : 0x18000;
}

static u64 g16_render_bg_base(void)
{
	return g16_drm_active_job ? 0x10000000000ULL + (g16_drm_active_job->render.bg.usc & ~63U) :
		0x10000158000ULL;
}

static u64 g16_render_eot_base(void)
{
	return g16_drm_active_job ? 0x10000000000ULL + (g16_drm_active_job->render.eot.usc & ~63U) :
		0x10000158480ULL;
}

#endif
