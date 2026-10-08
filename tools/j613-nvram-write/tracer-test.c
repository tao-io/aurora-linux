// SPDX-License-Identifier: MIT
#include <assert.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main(void)
{
	int fd = open(getenv("J613_NVRAM_TRACE_FILE"), O_RDWR);
	assert(fd >= 0);
	struct mtd_info_user info;
	assert(!ioctl(fd, MEMGETINFO, &info));
	assert(info.size == 0x100000 && info.erasesize == 4096);
	unsigned char aa = 0xaa, ff = 0xff, got = 0;
	assert(write(fd, &aa, 1) == 1);
	assert(lseek(fd, 0, SEEK_SET) == 0);
	assert(write(fd, &ff, 1) == -1);
	struct erase_info_user e = { .start = 0, .length = 4096 };
	assert(!ioctl(fd, MEMERASE, &e));
	assert(pread(fd, &got, 1, 0) == 1 && got == 0xff);
	e.start = 1;
	assert(ioctl(fd, MEMERASE, &e) == -1);
	e.start = 0xff000;
	e.length = 8192;
	assert(ioctl(fd, MEMERASE, &e) == -1);
	assert(lseek(fd, 0xfffff, SEEK_SET) == 0xfffff);
	assert(write(fd, "aa", 2) == -1);
	int null = open("/dev/null", O_RDWR);
	assert(null >= 0);
	assert(ioctl(null, MEMGETINFO, &info) == -1);
	close(null);
	close(fd);
	puts("PASS: private file MTD tracer, 0->1 refusal, erase/bounds, non-target passthrough");
}
