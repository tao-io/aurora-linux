// SPDX-License-Identifier: MIT
/* Three buffers filled once, then scanout-only flips. Run under both guards. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static unsigned int received, last_sequence;
static void flip(int fd, unsigned int sequence, unsigned int sec,
		 unsigned int usec, void *data)
{
	if (received && sequence <= last_sequence) {
		fprintf(stderr, "non-advancing flip sequence %u after %u\n", sequence, last_sequence);
		exit(1);
	}
	last_sequence = sequence;
	received++;
}

static void fail(const char *what)
{
	perror(what);
	exit(1);
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	drmEventContext events = { .version = 2, .page_flip_handler = flip };
	drmModeRes *res;
	drmModeConnector *connector = NULL;
	uint32_t fb[3], crtc;
	double started, elapsed;
	int fd, i;

	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/dri/cardN\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0)
		fail("open DRM");
	drmVersionPtr version = drmGetVersion(fd);
	if (!version || strcmp(version->name, "m4-dcp")) {
		fprintf(stderr, "refusing non-native KMS device\n");
		return 1;
	}
	drmFreeVersion(version);
	if (drmSetMaster(fd))
		fail("DRM master");
	res = drmModeGetResources(fd);
	if (!res || res->count_crtcs != 1)
		fail("get resources");
	crtc = res->crtcs[0];
	for (i = 0; i < res->count_connectors; i++) {
		connector = drmModeGetConnector(fd, res->connectors[i]);
		if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes)
			break;
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	if (!connector || connector->modes[0].hdisplay != 2560 || connector->modes[0].vdisplay != 1664)
		fail("native panel mode");
	for (i = 0; i < 3; i++) {
		struct drm_mode_create_dumb create = { .width = 2560, .height = 1664, .bpp = 32 };
		struct drm_mode_map_dumb map = {};
		uint32_t handles[4] = {}, strides[4] = {}, offsets[4] = {};
		uint32_t *pixels;
		unsigned int x, y;

		if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create))
			fail("create dumb");
		map.handle = create.handle;
		if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
			fail("map dumb");
		pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
		if (pixels == MAP_FAILED)
			fail("mmap dumb");
		/* XRGB alpha is deliberately zero. No CPU writes after this fill. */
		for (y = 0; y < create.height; y++)
			for (x = 0; x < create.width; x++) {
				unsigned int band = x * 8 / create.width;
				uint32_t color = ((band & 1) ? 0xff0000 : 0) |
						 ((band & 2) ? 0x00ff00 : 0) |
						 ((band & 4) ? 0x0000ff : 0);
				if (y < 100)
					color = 0xff << (8 * i);
				if (x < 4 || y < 4 || x >= create.width - 4 || y >= create.height - 4)
					color = 0xffffff;
				pixels[y * create.pitch / 4 + x] = color;
			}
		munmap(pixels, create.size);
		handles[0] = create.handle;
		strides[0] = create.pitch;
		if (drmModeAddFB2(fd, create.width, create.height, DRM_FORMAT_XRGB8888,
				  handles, strides, offsets, &fb[i], 0))
			fail("add framebuffer");
	}
	if (drmModeSetCrtc(fd, crtc, fb[0], 0, 0, &connector->connector_id, 1, &connector->modes[0]))
		fail("initial modeset");
	started = now();
	for (i = 0; i < 180; i++) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		if (drmModePageFlip(fd, crtc, fb[(i + 1) % 3], DRM_MODE_PAGE_FLIP_EVENT, NULL))
			fail("page flip");
		if (poll(&p, 1, 6000) != 1 || !(p.revents & POLLIN))
			fail("page flip completion timeout");
		if (drmHandleEvent(fd, &events) || received != (unsigned int)i + 1)
			fail("page flip event");
	}
	elapsed = now() - started;
	printf("PASS: %u completed native KMS flips in %.3f s (%.2f FPS), sequence=%u; three prefilled buffers, no per-frame CPU copy\n",
	       received, elapsed, received / elapsed, last_sequence);
	fflush(stdout);
	/* Disable through KMS before closing so the last buffer is retired safely. */
	if (drmModeSetCrtc(fd, crtc, 0, 0, 0, NULL, 0, NULL))
		fail("disable CRTC");
	close(fd);
	return 0;
}
