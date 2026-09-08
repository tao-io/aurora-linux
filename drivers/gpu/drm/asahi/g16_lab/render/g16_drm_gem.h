/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_GEM_H
#define G16_DRM_GEM_H

#include <drm/drm_prime.h>

struct g16_drm_gem {
	struct drm_gem_shmem_object shmem;
	u32 private_vm_id;
	bool wc_pages_pinned;
};

static struct g16_drm_gem *g16_drm_to_gem(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct g16_drm_gem, shmem);
}

static struct dma_buf *g16_drm_gem_export(struct drm_gem_object *obj, int flags)
{
	if (g16_drm_to_gem(obj)->private_vm_id)
		return ERR_PTR(-EINVAL);
	return drm_gem_prime_export(obj, flags);
}

static int g16_drm_gem_open(struct drm_gem_object *obj, struct drm_file *file);
static void g16_drm_gem_close(struct drm_gem_object *obj, struct drm_file *file);

static void g16_drm_gem_free(struct drm_gem_object *obj)
{
	struct g16_drm_gem *gem = g16_drm_to_gem(obj);

	if (gem->wc_pages_pinned)
		drm_gem_shmem_unpin(&gem->shmem);
	drm_gem_shmem_object_free(obj);
}

static int g16_drm_gem_prepare_wc(struct drm_gem_shmem_object *shmem)
{
	struct g16_drm_gem *gem = g16_drm_to_gem(&shmem->base);
	unsigned int i;
	int ret;

	if (!shmem->map_wc)
		return 0;
	/* ARM64 shmem pages are zeroed through the cached linear mapping. This
	 * lab installs physical UAT leaves directly, bypassing dma_map_sgtable's
	 * initial cache transfer. Flush that alias before any WC mmap/vmap or
	 * GPU access, or later eviction can overwrite the client's new contents.
	 * Retain the backing pages until object destruction: unpinning here could
	 * allow a replacement page to escape this one-time transition.
	 */
	ret = drm_gem_shmem_pin(shmem);
	if (ret)
		return ret;
	gem->wc_pages_pinned = true;
	for (i = 0; i < shmem->base.size / PAGE_SIZE; i++) {
		void *data = page_address(shmem->pages[i]);

		g16_lab_dcache_clean(data, PAGE_SIZE);
		g16_lab_dcache_invalidate(data, PAGE_SIZE);
	}
	return 0;
}

static const struct drm_gem_object_funcs g16_drm_gem_funcs = {
	.open = g16_drm_gem_open,
	.close = g16_drm_gem_close,
	.free = g16_drm_gem_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
	.export = g16_drm_gem_export,
};

static struct drm_gem_object *g16_drm_gem_allocate(struct drm_device *dev, size_t size)
{
	struct g16_drm_gem *obj = kzalloc(sizeof(*obj), GFP_KERNEL);

	if (!obj)
		return ERR_PTR(-ENOMEM);
	obj->shmem.base.funcs = &g16_drm_gem_funcs;
	return &obj->shmem.base;
}
#endif
