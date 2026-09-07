/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_H
#define G16_DRM_H

#include <linux/device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include "g16_drm_uapi.h"
#include "g16_drm_vm.h"

/* Memory-side bring-up first. Keep an explicit lab name so Mesa cannot
 * mistake this intermediate frontend for an executable Asahi device. */
static struct device *g16_drm_parent;
static struct drm_device *g16_drm_device;

struct g16_drm_file {
	struct mutex lock;
	u64 bytes_created;
	u32 objects_created;
	u32 next_vm_id;
	struct g16_drm_vm *vm;
};

static int g16_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct g16_drm_file *priv;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	mutex_init(&priv->lock);
	file->driver_priv = priv;
	return 0;
}

static void g16_drm_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct g16_drm_file *priv = file->driver_priv;

	g16_drm_vm_free(priv->vm);
	kfree(priv);
}

static int g16_drm_gem_create_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct g16_drm_gem_create *args = data;
	struct g16_drm_file *priv = file->driver_priv;
	struct drm_gem_shmem_object *shmem;
	u64 size;
	int ret;

	if (args->pad || args->vm_id || args->flags & ~G16_GEM_WRITEBACK ||
	    !args->size || args->size > SZ_64M)
		return -EINVAL;
	size = PAGE_ALIGN(args->size);
	mutex_lock(&priv->lock);
	/* Cumulative per-open budget for this bounded memory probe. */
	if (priv->objects_created == 64 || priv->bytes_created + size > SZ_256M) {
		ret = -ENOSPC;
		goto out;
	}
	shmem = drm_gem_shmem_create(dev, size);
	if (IS_ERR(shmem)) {
		ret = PTR_ERR(shmem);
		goto out;
	}
	shmem->map_wc = !(args->flags & G16_GEM_WRITEBACK);
	ret = drm_gem_handle_create(file, &shmem->base, &args->handle);
	drm_gem_object_put(&shmem->base);
	if (!ret) {
		priv->bytes_created += size;
		priv->objects_created++;
	}
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int g16_drm_gem_mmap_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct g16_drm_gem_mmap_offset *args = data;
	struct drm_gem_object *obj;

	if (args->flags)
		return -EINVAL;
	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	args->offset = drm_vma_node_offset_addr(&obj->vma_node);
	drm_gem_object_put(obj);
	return 0;
}

static int g16_drm_vm_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct g16_drm_vm_create *args = data;
	struct g16_drm_file *priv = file->driver_priv;
	struct g16_drm_vm *vm;
	int ret = 0;

	if (args->pad || (args->kernel_start | args->kernel_end) & (PAGE_SIZE - 1) ||
	    args->kernel_start < PAGE_SIZE || args->kernel_end > BIT_ULL(42) ||
	    args->kernel_end <= args->kernel_start || args->kernel_end - args->kernel_start < SZ_32M)
		return -EINVAL;
	mutex_lock(&priv->lock);
	if (priv->vm) {
		ret = -EBUSY;
		goto out;
	}
	if (priv->next_vm_id == U32_MAX) {
		ret = -ENOSPC;
		goto out;
	}
	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm) {
		ret = -ENOMEM;
		goto out;
	}
	INIT_LIST_HEAD(&vm->mappings);
	vm->id = ++priv->next_vm_id;
	vm->kernel_start = args->kernel_start;
	vm->kernel_end = args->kernel_end;
	priv->vm = vm;
	args->vm_id = vm->id;
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int g16_drm_vm_destroy_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct g16_drm_vm_destroy *args = data;
	struct g16_drm_file *priv = file->driver_priv;
	int ret = 0;

	if (args->pad)
		return -EINVAL;
	mutex_lock(&priv->lock);
	if (!priv->vm || priv->vm->id != args->vm_id) {
		ret = -ENOENT;
		goto out;
	}
	g16_drm_vm_free(priv->vm);
	priv->vm = NULL;
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int g16_drm_vm_bind_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct g16_drm_vm_bind *args = data;
	struct g16_drm_file *priv = file->driver_priv;
	struct g16_drm_bind_op op;
	int ret;

	if (args->pad || args->num_binds != 1 || args->stride < sizeof(op) || args->stride > 256)
		return -EINVAL;
	ret = copy_struct_from_user(&op, sizeof(op), u64_to_user_ptr(args->userptr), args->stride);
	if (ret)
		return ret;
	mutex_lock(&priv->lock);
	if (!priv->vm || priv->vm->id != args->vm_id)
		ret = -ENOENT;
	else
		ret = g16_drm_vm_bind_one(priv->vm, file, &op);
	mutex_unlock(&priv->lock);
	return ret;
}

static const struct drm_ioctl_desc g16_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(G16_VM_CREATE, g16_drm_vm_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(G16_VM_DESTROY, g16_drm_vm_destroy_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(G16_VM_BIND, g16_drm_vm_bind_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(G16_GEM_CREATE, g16_drm_gem_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(G16_GEM_MMAP_OFFSET, g16_drm_gem_mmap_ioctl, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(g16_drm_fops);

static const struct drm_driver g16_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.open = g16_drm_open,
	.postclose = g16_drm_postclose,
	.ioctls = g16_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(g16_drm_ioctls),
	.fops = &g16_drm_fops,
	.name = "g16-lab",
	.desc = "J713 experimental GPU memory frontend",
	.major = 0,
	.minor = 2,
};

static int g16_drm_start(void)
{
	int ret;

	if (!of_machine_is_compatible("apple,j713") ||
	    !of_machine_is_compatible("apple,t8132") || PAGE_SIZE != SZ_16K)
		return -ENODEV;
	g16_drm_parent = root_device_register("g16-lab-drm");
	if (IS_ERR(g16_drm_parent)) {
		ret = PTR_ERR(g16_drm_parent);
		g16_drm_parent = NULL;
		return ret;
	}
	g16_drm_device = drm_dev_alloc(&g16_drm_driver, g16_drm_parent);
	if (IS_ERR(g16_drm_device)) {
		ret = PTR_ERR(g16_drm_device);
		g16_drm_device = NULL;
		goto parent;
	}
	ret = drm_dev_register(g16_drm_device, 0);
	if (!ret)
		return 0;
	drm_dev_put(g16_drm_device);
	g16_drm_device = NULL;
parent:
	root_device_unregister(g16_drm_parent);
	g16_drm_parent = NULL;
	return ret;
}

static void g16_drm_stop(void)
{
	if (g16_drm_device) {
		drm_dev_unregister(g16_drm_device);
		drm_dev_put(g16_drm_device);
		g16_drm_device = NULL;
	}
	if (g16_drm_parent) {
		root_device_unregister(g16_drm_parent);
		g16_drm_parent = NULL;
	}
}
#endif
