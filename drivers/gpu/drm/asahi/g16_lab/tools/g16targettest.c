/* SPDX-License-Identifier: MIT */
/* Export the GPU's actual GEM render target and read it in an unprivileged
 * process after dma-buf fences complete. No Vulkan or compositor claim.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "g16_drm_uapi.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "pid %d line %d: %s: errno %d\n", getpid(), __LINE__, #x, errno); exit(1); } } while (0)

static void byte_send(int fd, unsigned char value)
{
   CHECK(write(fd, &value, 1) == 1);
}

static unsigned char byte_receive(int fd)
{
   struct pollfd p = {.fd = fd, .events = POLLIN};
   unsigned char value;
   CHECK(poll(&p, 1, 5000) == 1 && p.revents & POLLIN);
   CHECK(read(fd, &value, 1) == 1);
   return value;
}

static uint32_t rounded(unsigned numerator, unsigned denominator)
{
   return (numerator + denominator / 2) / denominator;
}

static void verify(const uint32_t *data, const struct g16_drm_lab_target *target, unsigned frame)
{
   CHECK(target->offset == 0x4000 && target->size == 0x44000);
   for (unsigned i = 0; i < target->offset / 4; ++i)
      CHECK(data[i] == 0);
   const uint32_t *pixels = data + target->offset / 4;
   for (unsigned y = 0; y < 256; ++y) {
      for (unsigned x = 0; x < 256; ++x) {
         unsigned index = ((y / 64) * 4 + x / 64) * 4096;
         for (unsigned bit = 0; bit < 6; ++bit)
            index |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1);
         uint32_t red = rounded((y > x ? y - x : 0) * 255, 256);
         uint32_t green = rounded((2 * x + 1) * 255, 512);
         uint32_t blue = rounded((511 - 2 * y) * 255, 512);
         uint32_t expected = x < y ? 0xff000000 | ((frame & 1) ?
            green << 16 | red << 8 | blue : blue << 16 | green << 8 | red) : 0;
         if (pixels[index] != expected) {
            fprintf(stderr, "frame=%u x=%u y=%u actual=%#x expected=%#x\n", frame, x, y, pixels[index], expected);
            exit(1);
         }
      }
   }
}

static void reader(int dmafd, const struct g16_drm_lab_target *target, int request, int reply)
{
   CHECK(setgroups(0, NULL) == 0 && setgid(1000) == 0 && setuid(1000) == 0);
   CHECK(geteuid() == 1000);
   const uint32_t *data = mmap(NULL, target->size, PROT_READ, MAP_SHARED, dmafd, 0);
   CHECK(data != MAP_FAILED);
   struct pollfd dma_poll = {.fd = dmafd, .events = POLLIN};
   CHECK(poll(&dma_poll, 1, 0) == 0);
   struct dma_buf_export_sync_file exported = {.flags = DMA_BUF_SYNC_READ, .fd = -1};
   CHECK(ioctl(dmafd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) == 0 && exported.fd >= 0);
   struct pollfd sync_poll = {.fd = exported.fd, .events = POLLIN};
   CHECK(poll(&sync_poll, 1, 0) == 0);
   byte_send(reply, 'R');
   for (unsigned frame = 0; frame < 8; ++frame) {
      CHECK(byte_receive(request) == frame + 1);
      CHECK(poll(&dma_poll, 1, 1000) == 1 && dma_poll.revents & POLLIN);
      if (!frame) {
         CHECK(poll(&sync_poll, 1, 1000) == 1 && sync_poll.revents & POLLIN);
         struct sync_file_info info = {0};
         CHECK(ioctl(exported.fd, SYNC_IOC_FILE_INFO, &info) == 0 && info.status == 1);
         CHECK(close(exported.fd) == 0);
      }
      struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
      CHECK(ioctl(dmafd, DMA_BUF_IOCTL_SYNC, &sync) == 0);
      verify(data, target, frame);
      sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      CHECK(ioctl(dmafd, DMA_BUF_IOCTL_SYNC, &sync) == 0);
      byte_send(reply, 'K');
   }
   CHECK(munmap((void *)data, target->size) == 0 && close(dmafd) == 0);
   CHECK(close(request) == 0 && close(reply) == 0);
   _exit(0);
}

int main(int argc, char **argv)
{
   CHECK(argc == 3 && geteuid() == 0);
   int drm = open(argv[1], O_RDWR | O_CLOEXEC);
   CHECK(drm >= 0);
   struct g16_drm_lab_target target = {0};
   CHECK(ioctl(drm, DRM_IOCTL_G16_LAB_TARGET, &target) == 0);
   CHECK(target.handle && target.width == 256 && target.height == 256 &&
         target.layout == G16_LAB_LAYOUT_MORTON_64_RGBA8);
   struct drm_prime_handle prime = {.handle = target.handle, .flags = DRM_CLOEXEC, .fd = -1};
   CHECK(ioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0 && prime.fd >= 0);
   struct drm_gem_close gem_close = {.handle = target.handle};
   CHECK(ioctl(drm, DRM_IOCTL_GEM_CLOSE, &gem_close) == 0 && close(drm) == 0);
   int request[2], reply[2];
   CHECK(pipe(request) == 0 && pipe(reply) == 0);
   pid_t child = fork();
   CHECK(child >= 0);
   if (!child) {
      close(request[1]); close(reply[0]);
      reader(prime.fd, &target, request[0], reply[1]);
   }
   CHECK(close(prime.fd) == 0 && close(request[0]) == 0 && close(reply[1]) == 0);
   CHECK(byte_receive(reply[0]) == 'R');
   for (unsigned sequence = 1; sequence <= 8; ++sequence) {
      int control = open(argv[2], O_WRONLY | O_CLOEXEC);
      CHECK(control >= 0);
      char payload[8];
      int length = snprintf(payload, sizeof(payload), "%u\n", sequence);
      CHECK(write(control, payload, length) == length && close(control) == 0);
      byte_send(request[1], sequence);
      CHECK(byte_receive(reply[0]) == 'K');
   }
   CHECK(close(request[1]) == 0 && close(reply[0]) == 0);
   int status;
   CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
   puts("{\"passed\":true,\"frames\":8,\"pixels_verified\":524288,\"reader_uid\":1000,"
        "\"exporter_closed_before_dispatch\":true,\"initial_fence_pending\":true,"
        "\"sync_file_completed\":true,\"dma_buf_poll_and_cpu_sync\":true}");
   return 0;
}
