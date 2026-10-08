// SPDX-License-Identifier: MIT
/* Linux LD_PRELOAD tracer: emulates MTD ONLY on a private regular-file copy.
 * It never opens a device. Metadata logs exclude data bytes and NVRAM values.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
static dev_t dev;
static ino_t ino;
static int initialized;
static int (*real_ioctl)(int, unsigned long, ...);
static ssize_t (*real_write)(int, const void *, size_t);
static int target(int fd)
{
	struct stat st;
	if (!initialized) {
		const char *p = getenv("J613_NVRAM_TRACE_FILE");
		if (!p || p[0] != '/' || lstat(p, &st) ||
		    !S_ISREG(st.st_mode) || st.st_size != 0x100000 ||
		    st.st_nlink != 1 || st.st_uid != getuid() ||
		    (st.st_mode & 077)) {
			_exit(120);
		}
		dev = st.st_dev;
		ino = st.st_ino;
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");
		real_write = dlsym(RTLD_NEXT, "write");
		if (!real_ioctl || !real_write)
			_exit(121);
		initialized = 1;
	}
	return !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_dev == dev &&
	       st.st_ino == ino;
}
int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (!target(fd))
		return real_ioctl(fd, req, arg);
	if (req == MEMGETINFO) {
		struct mtd_info_user *m = arg;
		*m = (struct mtd_info_user){ .type = MTD_NORFLASH,
					     .flags = MTD_CAP_NORFLASH,
					     .size = 0x100000,
					     .erasesize = 4096,
					     .writesize = 1 };
		return 0;
	}
	if (req == MEMERASE) {
		struct erase_info_user *e = arg;
		uint8_t ff[4096];
		if (!e->length || (e->start & 4095) || (e->length & 4095) ||
		    e->start >= 0x100000 || e->length > 0x100000 - e->start) {
			errno = EINVAL;
			return -1;
		}
		for (size_t i = 0; i < sizeof(ff); i++)
			ff[i] = 0xff;
		dprintf(2, "TRACE erase offset=0x%x length=0x%x\n", e->start,
			e->length);
		for (uint32_t p = 0; p < e->length; p += 4096)
			if (pwrite(fd, ff, 4096, e->start + p) != 4096)
				return -1;
		return 0;
	}
	errno = ENOTTY;
	return -1;
}
ssize_t write(int fd, const void *buf, size_t n)
{
	if (!target(fd))
		return real_write(fd, buf, n);
	off_t p = lseek(fd, 0, SEEK_CUR);
	const uint8_t *b = buf;
	uint8_t old[256];
	if (p < 0 || p >= 0x100000 || n > 0x100000 - p) {
		errno = EINVAL;
		return -1;
	}
	/* Match NOR semantics; fail on 0 -> 1 without an emulated erase. */
	for (size_t i = 0; i < n; i += sizeof(old)) {
		size_t len = n - i < sizeof(old) ? n - i : sizeof(old);
		if (pread(fd, old, len, p + i) != (ssize_t)len)
			return -1;
		for (size_t j = 0; j < len; j++)
			if ((old[j] & b[i + j]) != b[i + j]) {
				errno = EIO;
				return -1;
			}
	}
	dprintf(2, "TRACE write offset=0x%lx length=0x%zx\n", (unsigned long)p,
		n);
	return real_write(fd, buf, n);
}
