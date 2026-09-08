/* SPDX-License-Identifier: MIT */
/* No GPU submission: regress former GEM, mapped-byte and UAT-table cutoffs. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>
#include "g16_drm_uapi.h"

#define CHECK(x) do { if (!(x)) { perror(#x); exit(1); } } while (0)

static uint32_t create(int fd, uint64_t size)
{
   struct g16_drm_gem_create args = {.size = size, .flags = G16_GEM_WRITEBACK};
   CHECK(!ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &args));
   return args.handle;
}

static void bind(int fd, uint32_t vm, uint32_t handle, uint64_t addr, uint64_t size)
{
   struct g16_drm_bind_op op = {
      .flags = G16_BIND_READ, .handle = handle, .addr = addr, .range = size,
   };
   struct g16_drm_vm_bind args = {
      .vm_id = vm, .num_binds = 1, .stride = sizeof(op), .userptr = (uintptr_t)&op,
   };
   CHECK(!ioctl(fd, DRM_IOCTL_G16_VM_BIND, &args));
}

int main(int argc, char **argv)
{
   CHECK(argc == 2);
   int fd = open(argv[1], O_RDWR | O_CLOEXEC);
   CHECK(fd >= 0);
   uint32_t small = 0;
   for (unsigned i = 0; i < 8193; ++i)
      small = create(fd, 16384);
   /* WRITEBACK allocations are lazy here: this tests accounting, not a 1-GiB
    * physical allocation or GPU access. The old per-object bound was 64 MiB. */
   create(fd, UINT64_C(1088) << 20);
   struct g16_drm_gem_create overflow = {.size = UINT64_MAX};
   CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &overflow) == -1 && errno == EINVAL);

   struct g16_drm_vm_create vm = {
      .kernel_start = UINT64_C(0x3000000000), .kernel_end = UINT64_C(0x3800000000),
   };
   CHECK(!ioctl(fd, DRM_IOCTL_G16_VM_CREATE, &vm));
   /* Each 32-MiB step requires a distinct leaf table: exceed 64 table pages. */
   for (unsigned i = 0; i < 96; ++i)
      bind(fd, vm.vm_id, small, UINT64_C(0x20000000000) + ((uint64_t)i << 25), 16384);
   struct g16_drm_vm_destroy destroy = {.vm_id = vm.vm_id};
   CHECK(!ioctl(fd, DRM_IOCTL_G16_VM_DESTROY, &destroy));
   CHECK(!ioctl(fd, DRM_IOCTL_G16_VM_CREATE, &vm));
   uint32_t large = create(fd, UINT64_C(64) << 20);
   /* Alias the same physical pages: mapped-byte accounting exceeds 1 GiB. */
   for (unsigned i = 0; i < 17; ++i)
      bind(fd, vm.vm_id, large, UINT64_C(0x20000000000) + ((uint64_t)i << 26),
           UINT64_C(64) << 20);
   destroy.vm_id = vm.vm_id;
   CHECK(!ioctl(fd, DRM_IOCTL_G16_VM_DESTROY, &destroy));
   CHECK(!close(fd));
   puts("{\"passed\":true,\"live_objects\":8195,\"large_object_bytes\":1140850688,"
        "\"sparse_leaf_tables\":96,\"mapped_alias_bytes\":1140850688,\"gpu_submitted\":false}");
   return 0;
}
