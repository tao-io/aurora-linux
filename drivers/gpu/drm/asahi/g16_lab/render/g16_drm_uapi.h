/* SPDX-License-Identifier: MIT */
#ifndef G16_DRM_UAPI_H
#define G16_DRM_UAPI_H

#ifdef __KERNEL__
#include <uapi/drm/drm.h>
#else
#include <drm.h>
#endif

/* Wire-compatible subset of Aurora e58d46899eb1's Asahi UAPI. VM bindings currently
 * build private unpublished tables; no execution capability is implied. */
struct g16_drm_gem_create {
	__u64 size;
	__u32 flags;
	__u32 vm_id;
	__u32 handle;
	__u32 pad;
};

struct g16_drm_gem_mmap_offset {
	__u32 handle;
	__u32 flags;
	__u64 offset;
};

struct g16_drm_vm_create {
	__u64 kernel_start;
	__u64 kernel_end;
	__u32 vm_id;
	__u32 pad;
};

struct g16_drm_vm_destroy {
	__u32 vm_id;
	__u32 pad;
};

struct g16_drm_bind_op {
	__u32 flags;
	__u32 handle;
	__u64 offset;
	__u64 range;
	__u64 addr;
};

struct g16_drm_vm_bind {
	__u32 vm_id;
	__u32 num_binds;
	__u32 stride;
	__u32 pad;
	__u64 userptr;
};

/* Lab extension, deliberately outside the pinned Asahi ioctl subset. */
struct g16_drm_lab_target {
	__u32 flags;
	__u32 handle;
	__u64 size;
	__u32 offset;
	__u32 width;
	__u32 height;
	__u32 layout;
	__u64 pad;
};
#define G16_LAB_LAYOUT_MORTON_64_RGBA8 1
#define DRM_G16_LAB_TARGET 0x3e
#define DRM_IOCTL_G16_LAB_TARGET DRM_IOWR(DRM_COMMAND_BASE + DRM_G16_LAB_TARGET, struct g16_drm_lab_target)

#define DRM_G16_VM_CREATE 2
#define DRM_G16_VM_DESTROY 3
#define DRM_G16_VM_BIND 4
#define DRM_IOCTL_G16_VM_CREATE DRM_IOWR(DRM_COMMAND_BASE + DRM_G16_VM_CREATE, struct g16_drm_vm_create)
#define DRM_IOCTL_G16_VM_DESTROY DRM_IOW(DRM_COMMAND_BASE + DRM_G16_VM_DESTROY, struct g16_drm_vm_destroy)
#define DRM_IOCTL_G16_VM_BIND DRM_IOW(DRM_COMMAND_BASE + DRM_G16_VM_BIND, struct g16_drm_vm_bind)
#define G16_BIND_UNBIND 1
#define G16_BIND_READ 2
#define G16_BIND_WRITE 4

#define DRM_G16_GEM_CREATE 5
#define DRM_G16_GEM_MMAP_OFFSET 6
#define DRM_IOCTL_G16_GEM_CREATE DRM_IOWR(DRM_COMMAND_BASE + DRM_G16_GEM_CREATE, struct g16_drm_gem_create)
#define DRM_IOCTL_G16_GEM_MMAP_OFFSET DRM_IOWR(DRM_COMMAND_BASE + DRM_G16_GEM_MMAP_OFFSET, struct g16_drm_gem_mmap_offset)
#define G16_GEM_WRITEBACK 1
#define G16_GEM_VM_PRIVATE 2

#endif
