/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_GEM_H
#define G16_DRM_GEM_H

#include <drm/drm_prime.h>

struct g16_drm_gem {
	struct drm_gem_shmem_object shmem;
	u32 private_vm_id;
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

static const struct drm_gem_object_funcs g16_drm_gem_funcs = {
	.free = drm_gem_shmem_object_free,
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
