/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_JOB_H
#define G16_DRM_JOB_H

#include <uapi/drm/asahi_drm.h>

struct g16_drm_vm;

/* A synchronous, single-queue backend during bring-up. The submitting ioctl
 * holds the file/VM lock until completion, including firmware teardown on
 * failure. Consequently neither bindings nor their GEM references can vanish
 * while the GPU can still access them.
 */
struct g16_drm_job {
	struct drm_asahi_cmd_render render;
	struct g16_drm_vm *vm;
	struct g16_render_uat uat;
	struct dma_fence *fence;
	struct completion done;
	int result;
	bool published;
};

static struct g16_drm_job *g16_drm_active_job;
static int g16_drm_job_enter(struct g16_drm_job *job);
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
	return g16_drm_active_job ? 0x10000000000ULL + g16_drm_active_job->render.bg.usc :
		0x10000158000ULL;
}

static u64 g16_render_eot_base(void)
{
	return g16_drm_active_job ? 0x10000000000ULL + g16_drm_active_job->render.eot.usc :
		0x10000158480ULL;
}

#endif
