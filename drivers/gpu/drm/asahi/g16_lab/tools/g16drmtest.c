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

_Static_assert(DRM_IOCTL_G16_GEM_CREATE == DRM_IOCTL_ASAHI_GEM_CREATE, "create ABI");
_Static_assert(DRM_IOCTL_G16_GEM_MMAP_OFFSET == DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, "mmap ABI");

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s: %s\n", __LINE__, #x, strerror(errno)); exit(1); } } while (0)

static void close_handle(int fd, uint32_t handle)
{
   struct drm_gem_close close = {.handle = handle};
   CHECK(ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close) == 0);
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
   puts("{\"passed\":true,\"mapped_bytes_verified\":131072,\"invalid_creates_rejected\":5,"
        "\"foreign_handles_rejected\":true,\"foreign_mmaps_rejected\":true,"
        "\"mapping_survives_handle_close\":true,\"per_open_budget_verified\":true}");
   return 0;
}
