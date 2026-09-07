/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include "asahi_drm.h"
#include "g16_drm_uapi.h"

_Static_assert(DRM_IOCTL_G16_VM_CREATE == DRM_IOCTL_ASAHI_VM_CREATE, "VM create ABI");
_Static_assert(DRM_IOCTL_G16_VM_DESTROY == DRM_IOCTL_ASAHI_VM_DESTROY, "VM destroy ABI");
_Static_assert(DRM_IOCTL_G16_VM_BIND == DRM_IOCTL_ASAHI_VM_BIND, "VM bind ABI");
_Static_assert(sizeof(struct g16_drm_bind_op) == sizeof(struct drm_asahi_gem_bind_op), "bind op ABI");
_Static_assert(DRM_IOCTL_G16_GEM_CREATE == DRM_IOCTL_ASAHI_GEM_CREATE, "create ABI");
_Static_assert(DRM_IOCTL_G16_GEM_MMAP_OFFSET == DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, "mmap ABI");

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s: %s\n", __LINE__, #x, strerror(errno)); exit(1); } } while (0)

static void close_handle(int fd, uint32_t handle)
{
   struct drm_gem_close close = {.handle = handle};
   CHECK(ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close) == 0);
}

static int bind_op(int fd, uint32_t vm_id, struct g16_drm_bind_op *op)
{
   struct g16_drm_vm_bind bind = {
      .vm_id = vm_id, .num_binds = 1, .stride = sizeof(*op), .userptr = (uintptr_t)op,
   };
   return ioctl(fd, DRM_IOCTL_G16_VM_BIND, &bind);
}

static void test_vm(const char *path)
{
   int fd = open(path, O_RDWR | O_CLOEXEC);
   int other = open(path, O_RDWR | O_CLOEXEC);
   CHECK(fd >= 0 && other >= 0);
   struct g16_drm_vm_create vm = {
      .kernel_start = 0x3f800000000ull, .kernel_end = 0x40000000000ull,
   };
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_CREATE, &vm) == 0 && vm.vm_id);
   uint32_t first_id = vm.vm_id;
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_CREATE, &vm) == -1 && errno == EBUSY);
   struct g16_drm_gem_create create = {.size = 65536};
   CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &create) == 0);
   struct g16_drm_bind_op op = {
      .flags = G16_BIND_READ | G16_BIND_WRITE, .handle = create.handle,
      .range = 65536, .addr = 0x20000000000ull,
   };
   CHECK(bind_op(other, vm.vm_id, &op) == -1 && errno == ENOENT);
   CHECK(bind_op(fd, vm.vm_id + 1, &op) == -1 && errno == ENOENT);
   struct g16_drm_bind_op bad = op;
   bad.addr++;
   CHECK(bind_op(fd, vm.vm_id, &bad) == -1 && errno == EINVAL);
   bad = op; bad.range += 16384;
   CHECK(bind_op(fd, vm.vm_id, &bad) == -1 && errno == EINVAL);
   bad = op; bad.addr = vm.kernel_start;
   CHECK(bind_op(fd, vm.vm_id, &bad) == -1 && errno == EINVAL);
   bad = op; bad.offset = 16384;
   CHECK(bind_op(fd, vm.vm_id, &bad) == -1 && errno == EINVAL);
   struct { struct g16_drm_bind_op op; uint64_t extra; } extended = {op, 1};
   struct g16_drm_vm_bind bind = {
      .vm_id = vm.vm_id, .num_binds = 1, .stride = sizeof(extended), .userptr = (uintptr_t)&extended,
   };
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_BIND, &bind) == -1 && errno == E2BIG);
   CHECK(bind_op(fd, vm.vm_id, &op) == 0);
   CHECK(bind_op(fd, vm.vm_id, &op) == -1 && errno == EEXIST);
   /* Retain an alias with a nonzero object offset across unbinding the first. */
   struct g16_drm_bind_op alias = op;
   alias.addr += 0x400000000ull;
   alias.offset = 16384; alias.range = 32768; alias.flags = G16_BIND_READ;
   CHECK(bind_op(fd, vm.vm_id, &alias) == 0);
   close_handle(fd, create.handle);
   op.flags = G16_BIND_UNBIND; op.handle = 0;
   CHECK(bind_op(fd, vm.vm_id, &op) == 0);
   CHECK(bind_op(fd, vm.vm_id, &op) == -1 && errno == ENOENT);
   /* Destroy must release the remaining alias and its last GEM reference. */
   struct g16_drm_vm_destroy destroy = {.vm_id = vm.vm_id};
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_DESTROY, &destroy) == 0);
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_DESTROY, &destroy) == -1 && errno == ENOENT);
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_CREATE, &vm) == 0 && vm.vm_id != first_id);
   CHECK(bind_op(fd, first_id, &op) == -1 && errno == ENOENT);
   /* File close also tears down a VM with a mapped, handle-less object. */
   create.handle = 0;
   CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &create) == 0);
   alias.handle = create.handle;
   CHECK(bind_op(fd, vm.vm_id, &alias) == 0);
   close_handle(fd, create.handle);
   CHECK(close(fd) == 0 && close(other) == 0);
}

static void test_private_gem(const char *path)
{
   int fd = open(path, O_RDWR | O_CLOEXEC);
   CHECK(fd >= 0);
   struct g16_drm_vm_create vm = {.kernel_start = 0x3000000000, .kernel_end = 0x3800000000};
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_CREATE, &vm) == 0);
   struct g16_drm_gem_create create = {.size = 16384,
      .flags = G16_GEM_VM_PRIVATE | G16_GEM_WRITEBACK, .vm_id = vm.vm_id + 1};
   CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &create) == -1 && errno == ENOENT);
   create.vm_id = vm.vm_id;
   CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &create) == 0);
   struct drm_prime_handle prime = {.handle = create.handle, .flags = DRM_CLOEXEC, .fd = -1};
   CHECK(ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == -1 && errno == EINVAL);
   struct g16_drm_bind_op op = {.flags = G16_BIND_READ, .handle = create.handle,
      .range = 16384, .addr = 0x10000000000};
   CHECK(bind_op(fd, vm.vm_id, &op) == 0);
   struct g16_drm_vm_destroy destroy = {.vm_id = vm.vm_id};
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_DESTROY, &destroy) == 0);
   CHECK(ioctl(fd, DRM_IOCTL_G16_VM_CREATE, &vm) == 0 && vm.vm_id != create.vm_id);
   CHECK(bind_op(fd, vm.vm_id, &op) == -1 && errno == EINVAL);
   close_handle(fd, create.handle);
   CHECK(close(fd) == 0);
}

int main(int argc, char **argv)
{
   CHECK(argc == 2);
   int fd = open(argv[1], O_RDWR | O_CLOEXEC);
   int other = open(argv[1], O_RDWR | O_CLOEXEC);
   CHECK(fd >= 0 && other >= 0);
   drmVersionPtr version = drmGetVersion(fd);
   CHECK(version && !strcmp(version->name, "g16-lab"));
   drmFreeVersion(version);
   drmDevicePtr device = NULL;
   CHECK(drmGetDevice2(fd, 0, &device) == 0 && device && device->bustype == DRM_BUS_PLATFORM);
   CHECK(device->available_nodes & (1 << DRM_NODE_RENDER));
   drmFreeDevice(&device);
   struct drm_asahi_params_global params;
   struct drm_asahi_get_params query = {.pointer = (uintptr_t)&params, .size = sizeof(params)};
   CHECK(ioctl(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &query) == -1 && errno == EAGAIN);

   struct g16_drm_gem_create invalid[] = {
      {.size = 0}, {.size = UINT64_MAX}, {.size = 16384, .flags = 2},
      {.size = 16384, .pad = 1}, {.size = 16384, .vm_id = 1},
   };
   for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &invalid[i]) == -1);
      CHECK(errno == EINVAL);
   }

   for (unsigned flags = 0; flags <= G16_GEM_WRITEBACK; ++flags) {
      struct g16_drm_gem_create create = {.size = 65535, .flags = flags};
      CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &create) == 0 && create.handle);
      struct g16_drm_gem_mmap_offset map = {.handle = create.handle};
      CHECK(ioctl(other, DRM_IOCTL_G16_GEM_MMAP_OFFSET, &map) == -1 && errno == ENOENT);
      CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_MMAP_OFFSET, &map) == 0 && map.offset);
      void *foreign = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, other, map.offset);
      CHECK(foreign == MAP_FAILED && (errno == EACCES || errno == EINVAL));
      uint8_t *first = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
      CHECK(first != MAP_FAILED);
      for (unsigned i = 0; i < 65536; ++i)
         CHECK(first[i] == 0);
      for (unsigned i = 0; i < 65536; ++i)
         first[i] = (i * 131 + 17) & 255;
      uint8_t *second = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
      CHECK(second != MAP_FAILED);
      close_handle(fd, create.handle);
      CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_MMAP_OFFSET, &map) == -1 && errno == ENOENT);
      CHECK(munmap(first, 65536) == 0);
      for (unsigned i = 0; i < 65536; ++i)
         CHECK(second[i] == ((i * 131 + 17) & 255));
      CHECK(munmap(second, 65536) == 0);
   }

   for (unsigned i = 2; i < 64; ++i) {
      struct g16_drm_gem_create create = {.size = 1};
      CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &create) == 0);
      close_handle(fd, create.handle);
   }
   struct g16_drm_gem_create exhausted = {.size = 1};
   CHECK(ioctl(fd, DRM_IOCTL_G16_GEM_CREATE, &exhausted) == -1 && errno == ENOSPC);
   CHECK(close(fd) == 0 && close(other) == 0);
   test_vm(argv[1]);
   test_private_gem(argv[1]);
   puts("{\"passed\":true,\"mapped_bytes_verified\":131072,\"invalid_creates_rejected\":5,"
        "\"foreign_handles_rejected\":true,\"foreign_mmaps_rejected\":true,"
        "\"mapping_survives_handle_close\":true,\"per_open_budget_verified\":true,\"private_vm_bindings_verified\":true,"
        "\"platform_discovery_verified\":true,\"private_gem_exports_rejected\":true,\"private_gem_vm_lifetime_verified\":true}");
   return 0;
}
