/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_BUFFER_H
#define G16_RENDER_BUFFER_H

#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_shmem_helper.h>

/* The lab render target can use the same GEM pages exposed to userspace.
 * Only this owned target is exported; command/firmware memory stays private.
 */
static struct drm_device *g16_drm_device;
static struct drm_gem_object *g16_render_target_obj;
static struct iosys_map g16_render_target_map;
static struct dma_fence *g16_render_target_fence;
static u64 g16_render_fence_context;
static u64 g16_render_fence_sequence;

struct g16_target_fence {
	struct dma_fence base;
	spinlock_t lock;
	bool module_reference;
};

static const char *g16_target_fence_driver(struct dma_fence *fence)
{
	return "g16-lab";
}

static const char *g16_target_fence_timeline(struct dma_fence *fence)
{
	return "render-target";
}

static void g16_target_fence_free(struct dma_fence *fence)
{
	struct g16_target_fence *target = container_of(fence, struct g16_target_fence, base);
	bool module_reference = target->module_reference;

	kfree(target);
	if (module_reference)
		module_put(THIS_MODULE);
}

static const struct dma_fence_ops g16_target_fence_ops = {
	.get_driver_name = g16_target_fence_driver,
	.get_timeline_name = g16_target_fence_timeline,
	.release = g16_target_fence_free,
};

static int g16_render_buffer_begin(void)
{
	struct g16_target_fence *fence;
	int ret;

	if (!g16_render_target_obj || g16_render_target_fence)
		return 0;
	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return -ENOMEM;
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &g16_target_fence_ops, &fence->lock,
		g16_render_fence_context, ++g16_render_fence_sequence);
	ret = dma_resv_lock_interruptible(g16_render_target_obj->resv, NULL);
	if (ret)
		goto put;
	ret = dma_resv_reserve_fences(g16_render_target_obj->resv, 1);
	if (!ret) {
		dma_resv_add_fence(g16_render_target_obj->resv, &fence->base, DMA_RESV_USAGE_WRITE);
		g16_render_target_fence = &fence->base;
	}
	dma_resv_unlock(g16_render_target_obj->resv);
	if (!ret)
		return 0;
put:
	dma_fence_put(&fence->base);
	return ret;
}

static void g16_render_buffer_end(int result)
{
	struct dma_fence *fence = g16_render_target_fence;

	if (!fence)
		return;
	if (result < 0)
		dma_fence_set_error(fence, result);
	dma_fence_signal(fence);
	g16_render_target_fence = NULL;
	dma_fence_put(fence);
}

static void g16_render_buffer_release(void)
{
	g16_render_buffer_end(-ECANCELED);
	if (g16_render_target_obj) {
		drm_gem_vunmap(g16_render_target_obj, &g16_render_target_map);
		drm_gem_object_put(g16_render_target_obj);
		g16_render_target_obj = NULL;
	}
	iosys_map_clear(&g16_render_target_map);
}

static int g16_render_buffer_allocate(void **data, size_t size)
{
	struct drm_gem_shmem_object *shmem;
	int ret;

	if (!render_drm_target)
		return 0;
	if (!g16_drm_device || g16_render_target_obj)
		return -EINVAL;
	shmem = drm_gem_shmem_create(g16_drm_device, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	shmem->map_wc = true;
	ret = drm_gem_vmap(&shmem->base, &g16_render_target_map);
	if (ret) {
		drm_gem_object_put(&shmem->base);
		return ret;
	}
	g16_render_target_obj = &shmem->base;
	g16_render_fence_context = dma_fence_context_alloc(1);
	/* Publish an unsignalled first-frame fence before readers can export. */
	ret = g16_render_buffer_begin();
	if (ret) {
		g16_render_buffer_release();
		return ret;
	}
	memcpy(g16_render_target_map.vaddr, *data, size);
	vfree(*data);
	*data = g16_render_target_map.vaddr;
	return 0;
}
#endif
