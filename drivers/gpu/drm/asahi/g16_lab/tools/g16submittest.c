/* SPDX-License-Identifier: MIT */
/* Exercise the real Asahi submit wire ABI with client-created GEM objects.
 * Initial shader/stream fixtures come from the qualified M4 lab preparation;
 * the client owns every submitted resource and changes vertex colors per draw.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <asahi_drm.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s: errno %d (%s)\n", __LINE__, #x, errno, strerror(errno)); exit(1); } } while (0)
#define FAILS(x, e) do { errno = 0; CHECK((x) == -1 && errno == (e)); } while (0)

struct resource {
   uint64_t va, size;
   uint32_t handle;
   unsigned char *cpu;
};

static void exact_write(int fd, const void *data, size_t size)
{
   while (size) {
      ssize_t put = write(fd, data, size);
      CHECK(put > 0);
      data = (const char *)data + put;
      size -= put;
   }
}

static void verify(const uint32_t *data, unsigned frame, const char *output)
{
   for (unsigned i = 0; i < 0x4000 / 4; ++i)
      CHECK(data[i] == 0);
   const uint32_t *pixels = data + 0x4000 / 4;
   unsigned r = (frame * 255 + 4) / 8, g = ((7 - frame) * 255 + 4) / 8;
   uint32_t color = 0xff200000 | g << 8 | r;
   uint32_t linear[256 * 256];
   for (unsigned y = 0; y < 256; ++y) {
      for (unsigned x = 0; x < 256; ++x) {
         unsigned index = ((y / 64) * 4 + x / 64) * 4096;
         for (unsigned bit = 0; bit < 6; ++bit)
            index |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1);
         uint32_t expected = x < y ? color : 0;
         linear[y * 256 + x] = pixels[index];
         if (pixels[index] != expected) {
            fprintf(stderr, "frame=%u x=%u y=%u actual=%#x expected=%#x\n", frame, x, y, pixels[index], expected);
            exit(1);
         }
      }
   }
   char path[4096];
   CHECK(snprintf(path, sizeof(path), "%s/client-frame-%02u.rgba", output, frame) < (int)sizeof(path));
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   CHECK(fd >= 0);
   exact_write(fd, linear, sizeof(linear));
   CHECK(close(fd) == 0);
}

int main(int argc, char **argv)
{
   CHECK(argc == 4 && geteuid() == 0 && sysconf(_SC_PAGESIZE) == 16384);
   int drm = open(argv[1], O_RDWR | O_CLOEXEC);
   CHECK(drm >= 0);
   struct drm_asahi_vm_create vm = {.kernel_start = 0x3000000000, .kernel_end = 0x3020000000};
   CHECK(ioctl(drm, DRM_IOCTL_ASAHI_VM_CREATE, &vm) == 0);
   struct drm_asahi_queue_create queue = {.vm_id = vm.vm_id,
      .priority = DRM_ASAHI_PRIORITY_MEDIUM, .usc_exec_base = 0x10000000000};
   CHECK(ioctl(drm, DRM_IOCTL_ASAHI_QUEUE_CREATE, &queue) == 0 && queue.queue_id);
   struct drm_asahi_vm_destroy destroy_vm = {.vm_id = vm.vm_id};
   FAILS(ioctl(drm, DRM_IOCTL_ASAHI_VM_DESTROY, &destroy_vm), EBUSY);

   struct resource resources[34], *vertices = NULL, *target = NULL;
   unsigned count = 0;
   DIR *directory = opendir(argv[2]);
   CHECK(directory);
   struct dirent *entry;
   while ((entry = readdir(directory))) {
      uint64_t va;
      int end = 0;
      if (sscanf(entry->d_name, "resource-%" SCNx64 ".bin%n", &va, &end) != 1 ||
          !end || entry->d_name[end])
         continue;
      CHECK(count < 34);
      int input = openat(dirfd(directory), entry->d_name, O_RDONLY | O_CLOEXEC);
      CHECK(input >= 0);
      /* debugfs blobs have st_size == 0; read through EOF instead. */
      unsigned char *blob = malloc(16 * 1024 * 1024);
      CHECK(blob);
      size_t size = 0;
      for (;;) {
         CHECK(size < 16 * 1024 * 1024);
         ssize_t got = read(input, blob + size, 16 * 1024 * 1024 - size);
         CHECK(got >= 0);
         if (!got)
            break;
         size += got;
      }
      CHECK(size && !(size & 0x3fff));
      struct drm_asahi_gem_create gem = {.size = size};
      CHECK(ioctl(drm, DRM_IOCTL_ASAHI_GEM_CREATE, &gem) == 0);
      struct drm_asahi_gem_mmap_offset offset = {.handle = gem.handle};
      CHECK(ioctl(drm, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &offset) == 0);
      struct resource *resource = &resources[count++];
      *resource = (struct resource){.va = va, .size = gem.size, .handle = gem.handle};
      resource->cpu = mmap(NULL, gem.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, offset.offset);
      CHECK(resource->cpu != MAP_FAILED);
      memcpy(resource->cpu, blob, size);
      free(blob);
      CHECK(close(input) == 0);
      struct drm_asahi_gem_bind_op op = {.handle = gem.handle, .range = gem.size, .addr = va,
         .flags = DRM_ASAHI_BIND_READ};
      if (va == 0x1000000000 || va == 0x10001000000)
         op.flags |= DRM_ASAHI_BIND_WRITE;
      struct drm_asahi_vm_bind bind = {.vm_id = vm.vm_id, .num_binds = 1,
         .stride = sizeof(op), .userptr = (uintptr_t)&op};
      CHECK(ioctl(drm, DRM_IOCTL_ASAHI_VM_BIND, &bind) == 0);
      if (va == 0x10000148000)
         vertices = resource;
      if (va == 0x10001000000)
         target = resource;
   }
   CHECK(closedir(directory) == 0 && count == 34 && vertices && target && target->size == 0x44000);
   struct drm_prime_handle prime = {.handle = target->handle, .flags = DRM_CLOEXEC, .fd = -1};
   CHECK(ioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0 && prime.fd >= 0);
   struct drm_syncobj_create dependency = {.flags = DRM_SYNCOBJ_CREATE_SIGNALED}, finished = {0};
   CHECK(ioctl(drm, DRM_IOCTL_SYNCOBJ_CREATE, &dependency) == 0);
   CHECK(ioctl(drm, DRM_IOCTL_SYNCOBJ_CREATE, &finished) == 0);
   struct drm_syncobj_create timeline = {0};
   CHECK(ioctl(drm, DRM_IOCTL_SYNCOBJ_CREATE, &timeline) == 0);
   uint64_t point = 1;
   struct drm_syncobj_timeline_array initial = {.handles = (uintptr_t)&timeline.handle,
      .points = (uintptr_t)&point, .count_handles = 1};
   CHECK(ioctl(drm, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &initial) == 0);
   struct drm_asahi_sync syncs[] = {{.handle = dependency.handle},
      {.handle = timeline.handle, .sync_type = DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ, .timeline_value = 1},
      {.handle = finished.handle},
      {.handle = timeline.handle, .sync_type = DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ, .timeline_value = 2}};
   struct {
      struct drm_asahi_cmd_header header;
      struct drm_asahi_cmd_render render;
   } command = {
      .header = {.cmd_type = DRM_ASAHI_CMD_RENDER, .size = sizeof(command.render),
                 .vdm_barrier = DRM_ASAHI_BARRIER_NONE, .cdm_barrier = DRM_ASAHI_BARRIER_NONE},
      .render = {.vdm_ctrl_stream_base = 0x100001c000, .width_px = 256, .height_px = 256,
         .layers = 1, .utile_width_px = 32, .utile_height_px = 32, .samples = 1, .sample_size_B = 4,
         .bg.usc = 0x158000, .eot.usc = 0x158480},
   };
   struct drm_asahi_submit submit = {.queue_id = queue.queue_id, .cmdbuf = (uintptr_t)&command,
      .cmdbuf_size = sizeof(command), .syncs = (uintptr_t)syncs, .in_sync_count = 2, .out_sync_count = 2};
   command.render.samples = 4;
   FAILS(ioctl(drm, DRM_IOCTL_ASAHI_SUBMIT, &submit), EOPNOTSUPP);
   command.render.samples = 1;
   command.render.vdm_ctrl_stream_base += 0x800000;
   FAILS(ioctl(drm, DRM_IOCTL_ASAHI_SUBMIT, &submit), EFAULT);
   command.render.vdm_ctrl_stream_base -= 0x800000;
   command.render.flags = 0x80000000;
   FAILS(ioctl(drm, DRM_IOCTL_ASAHI_SUBMIT, &submit), EOPNOTSUPP);
   command.render.flags = 0;
   int other = open(argv[1], O_RDWR | O_CLOEXEC);
   CHECK(other >= 0);
   struct drm_asahi_submit foreign = submit;
   foreign.in_sync_count = foreign.out_sync_count = 0;
   FAILS(ioctl(other, DRM_IOCTL_ASAHI_SUBMIT, &foreign), ENOENT);
   CHECK(close(other) == 0);

   int retained_fence = -1;
   for (unsigned frame = 0; frame < 8; ++frame) {
      syncs[1].timeline_value = frame + 1;
      syncs[3].timeline_value = frame + 2;
      float r = frame / 8.0f, g = (7 - frame) / 8.0f;
      for (unsigned vertex = 0; vertex < 3; ++vertex) {
         memcpy(vertices->cpu + 0x5108 + vertex * 16, &r, 4);
         memcpy(vertices->cpu + 0x510c + vertex * 16, &g, 4);
      }
      memset(target->cpu, 0, target->size);
      CHECK(ioctl(drm, DRM_IOCTL_ASAHI_SUBMIT, &submit) == 0);
      point = 0;
      CHECK(ioctl(drm, DRM_IOCTL_SYNCOBJ_QUERY, &initial) == 0 && point == frame + 2);
      struct drm_syncobj_handle syncfd = {.handle = finished.handle, .fd = -1,
         .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE};
      CHECK(ioctl(drm, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &syncfd) == 0 && syncfd.fd >= 0);
      struct pollfd p = {.fd = syncfd.fd, .events = POLLIN};
      CHECK(poll(&p, 1, 1000) == 1 && p.revents & POLLIN);
      struct sync_file_info info = {0};
      CHECK(ioctl(syncfd.fd, SYNC_IOC_FILE_INFO, &info) == 0 && info.status == 1);
      if (frame == 7)
         retained_fence = syncfd.fd;
      else
         CHECK(close(syncfd.fd) == 0);
      struct dma_buf_sync cpu = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
      CHECK(ioctl(prime.fd, DMA_BUF_IOCTL_SYNC, &cpu) == 0);
      verify((uint32_t *)target->cpu, frame, argv[3]);
      cpu.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      CHECK(ioctl(prime.fd, DMA_BUF_IOCTL_SYNC, &cpu) == 0);
      fprintf(stderr, "client frame %u: 65536 pixels verified, sync-file signalled\n", frame);
   }
   /* Asahi closes remove mappings after the last handle. Completed exported
    * fences and the target dma-buf remain valid independently. */
   for (unsigned i = 0; i < count; ++i) {
      struct drm_gem_close close_gem = {.handle = resources[i].handle};
      CHECK(ioctl(drm, DRM_IOCTL_GEM_CLOSE, &close_gem) == 0);
   }
   FAILS(ioctl(drm, DRM_IOCTL_ASAHI_SUBMIT, &submit), EFAULT);
   struct drm_asahi_queue_destroy destroy_queue = {.queue_id = queue.queue_id};
   CHECK(ioctl(drm, DRM_IOCTL_ASAHI_QUEUE_DESTROY, &destroy_queue) == 0);
   FAILS(ioctl(drm, DRM_IOCTL_ASAHI_QUEUE_DESTROY, &destroy_queue), ENOENT);
   CHECK(ioctl(drm, DRM_IOCTL_ASAHI_VM_DESTROY, &destroy_vm) == 0);
   for (unsigned i = 0; i < count; ++i)
      CHECK(munmap(resources[i].cpu, resources[i].size) == 0);
   CHECK(close(prime.fd) == 0 && close(drm) == 0);
   FILE *references = fopen("/sys/module/g16_lab/refcnt", "r");
   unsigned refs = 0;
   CHECK(references && fscanf(references, "%u", &refs) == 1 && refs >= 1);
   CHECK(fclose(references) == 0 && retained_fence >= 0 && close(retained_fence) == 0);
   puts("{\"passed\":true,\"client_gem_resources\":34,\"frames\":8,\"pixels_verified\":524288,"
        "\"binary_sync_files_verified\":8,\"last_handle_close_unmaps\":true,"
        "\"timeline_points_verified\":8,"
        "\"standalone_sync_file_pins_module\":true,"
        "\"invalid_requests_rejected\":7,\"vulkan\":false}");
   return 0;
}
