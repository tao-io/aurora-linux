/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#include "g16_render_sequence.h"
int main(void)
{
	assert(g16_render_job_stamp(0) == 0x100);
	assert(g16_render_job_stamp(65534) == 0xffff00);
	assert(g16_render_job_stamp(65535) == 0x1000000);
	assert(g16_render_job_stamp(0xfffffe) == 0xffffff00);
	assert(g16_render_job_stamp(0xffffff) == 0x100);
	assert(g16_render_job_stamp(0x1000000) == 0x200);
	u32 previous = 0;
	for (u64 i = 0; i < 0x1000010; i++) {
		u32 stamp = g16_render_job_stamp(i);
		assert(stamp && !(stamp & 255) && stamp != previous);
		previous = stamp;
	}
	assert(g16_render_job_stamp(UINT64_MAX));
	puts("nonzero firmware stamp rollover: PASS");
}
