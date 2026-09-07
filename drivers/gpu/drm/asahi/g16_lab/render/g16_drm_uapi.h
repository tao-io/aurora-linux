/* SPDX-License-Identifier: MIT */
#ifndef G16_DRM_UAPI_H
#define G16_DRM_UAPI_H

#ifdef __KERNEL__
#include <uapi/drm/drm.h>
#else
#include <drm.h>
#endif

/* Wire-compatible subset of Aurora e58d46899eb1's Asahi UAPI. No VM or
 * execution capability is implied by these buffer operations. */
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

#define DRM_G16_GEM_CREATE 5
#define DRM_G16_GEM_MMAP_OFFSET 6
#define DRM_IOCTL_G16_GEM_CREATE DRM_IOWR(DRM_COMMAND_BASE + DRM_G16_GEM_CREATE, struct g16_drm_gem_create)
#define DRM_IOCTL_G16_GEM_MMAP_OFFSET DRM_IOWR(DRM_COMMAND_BASE + DRM_G16_GEM_MMAP_OFFSET, struct g16_drm_gem_mmap_offset)
#define G16_GEM_WRITEBACK 1

#endif
