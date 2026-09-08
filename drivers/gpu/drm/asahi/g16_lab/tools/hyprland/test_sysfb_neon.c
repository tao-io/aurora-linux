/* SPDX-License-Identifier: GPL-2.0-only */
/* User-space checks for the exact kernel framebuffer conversion assembly. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
void drm_sysfb_neon_xrgb2101010(void *, const void *, unsigned);
static uint32_t reference(uint32_t p)
{
	p = ((p & 255) << 2) | ((p & 0xff00) << 4) | ((p & 0xff0000) << 6);
	return p | ((p >> 8) & 0x00300c03);
}
int main(void)
{
	uint32_t src[1024], dst[1024];
	for (unsigned base = 0; base < (1u << 24); base += 1024) {
		for (unsigned i = 0; i < 1024; i++)
			src[i] = (base + i) | ((i * 71u) << 24);
		drm_sysfb_neon_xrgb2101010(dst, src, 1024);
		for (unsigned i = 0; i < 1024; i++)
			assert(dst[i] == reference(src[i]));
	}
	unsigned char a[1200], b[1200], want[1200];
	for (unsigned i = 0; i < sizeof(a); i++)
		a[i] = i * 37u + 13u;
	for (unsigned n = 0; n <= 257; n++)
		for (unsigned off = 0; off < 16; off++) {
			memset(b, 0xa5, sizeof(b));
			memcpy(want, b, sizeof(b));
			for (unsigned i = 0; i < n; i++) {
				uint32_t p;
				memcpy(&p, a + off + 4 * i, 4);
				p = reference(p);
				memcpy(want + 15 - off + 4 * i, &p, 4);
			}
			drm_sysfb_neon_xrgb2101010(b + 15 - off, a + off, n);
			assert(!memcmp(b, want, sizeof(b)));
		}
	size_t page = sysconf(_SC_PAGESIZE);
	unsigned char *s = mmap(0, page * 2, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char *d = mmap(0, page * 2, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(s != MAP_FAILED && d != MAP_FAILED);
	assert(!mprotect(s + page, page, PROT_NONE));
	assert(!mprotect(d + page, page, PROT_NONE));
	memset(s, 0x81, page);
	for (unsigned n = 0; n <= 1024; n++) {
		drm_sysfb_neon_xrgb2101010(d + page - 4 * n, s + page - 4 * n,
					   n);
		for (unsigned i = 0; i < n; i++) {
			uint32_t p;
			memcpy(&p, d + page - 4 * n + 4 * i, 4);
			assert(p == reference(0x81818181));
		}
	}
	munmap(s, page * 2);
	munmap(d, page * 2);
	puts("PASS: all 16,777,216 RGB colors; 4,128 unaligned/tail cases; 1,025 guard-page boundaries");
}
