// SPDX-License-Identifier: GPL-2.0-only
/* Host-side ABI and rejection tests; never access the GPU. */
#include <assert.h>
#include <stdio.h>
#include "g16_render_pack.h"

static void test_register_bounds(void)
{
	u8 command[0x1000], before[sizeof(command)];
	struct g16_render_register regs[129];
	unsigned int stage, i;

	for (i = 0; i < 129; i++)
		regs[i] = (struct g16_render_register){0x1748, 1, 0x123456789abcdef0ULL};
	for (stage = 0; stage < 2; stage++) {
		unsigned int base = stage ? 0x80 : 0x40;
		unsigned int limit = stage ? 126 : 125;

		memset(command, 0xa5, sizeof(command));
		assert(!g16_render_pack_registers(command, sizeof(command), stage,
						0x6f00004000, regs, limit));
		assert(g16_render_get(command + base, 4) == 0x1749);
		assert(g16_render_get(command + base + 4, 8) == regs[0].value);
		assert(g16_render_get(command + base + 0x700, 8) == 0x6f00004000ULL + base);
		assert(g16_render_get(command + base + 0x708, 2) == limit);
		assert(g16_render_get(command + base + 0x70a, 2) == limit * 12);
		for (i = base + limit * 12; i < base + 0x600; i++)
			assert(command[i] == 0);
		for (i = base + 0x600; i < base + 0x700; i++)
			assert(command[i] == 0xa5);
		memcpy(before, command, sizeof(command));
		assert(g16_render_pack_registers(command, sizeof(command), stage,
				0x6f00004000, regs, limit + 1) == -E2BIG);
		assert(!memcmp(command, before, sizeof(command)));
		regs[0].reg |= 2;
		assert(g16_render_pack_registers(command, sizeof(command), stage,
				0x6f00004000, regs, 1) == -EINVAL);
		assert(!memcmp(command, before, sizeof(command)));
		regs[0].reg &= ~2U;
	}
}

static void test_pages(void)
{
	u64 pages[] = {0x7000000000, 0x7000008000, 0xffffffff8000};
	u8 out[12], before[12];

	memset(out, 0xa5, sizeof(out));
	memcpy(before, out, sizeof(out));
	assert(g16_render_pack_page_list(out, sizeof(out), pages, 3) == -ERANGE);
	assert(!memcmp(out, before, sizeof(out))); /* No partial writes on bad input. */
	pages[2] = 0xffffff8000;
	assert(!g16_render_pack_page_list(out, sizeof(out), pages, 3));
	assert(g16_render_get(out + 0, 4) == 0xe00000);
	assert(g16_render_get(out + 4, 4) == 0xe00001);
	assert(g16_render_get(out + 8, 4) == 0x1ffffff);
}

static void test_sku(void)
{
	struct g16_render_sku_args a = {
		.command_va = 0xfffffc0010000000ULL,
		.sku_va = 0xfffffc0010010000ULL,
		.queue_va = 0xfffffc0010020000ULL,
		.slot_array_va = 0xfffffc0010030004ULL,
		.pb_slot_va = 0xfffffc0010040000ULL,
		.buffer_info_va = 0xfffffc0010050000ULL,
		.stamp_va = 0xfffffc0010060000ULL,
		.context_id = 5, .submission_slot = 2, .uuid = 0x12345678,
		.stamp = 0x100, .stamp_slot = 7, .fragment_increment = 1,
	};
	u8 storage[0x401], before[sizeof(storage)];
	u8 *sku = storage + 1; /* Exercise all stores at deliberately odd alignment. */
	unsigned int engine;

	memset(a.uma, 0x5a, sizeof(a.uma));
	for (engine = 0; engine < 2; engine++) {
		unsigned int finish = engine ? 0x1f0 : 0x1d0;
		unsigned int uma = engine ? 0x1b0 : 0x190;
		unsigned int restart = engine ? 0xa0 : 0x80;
		unsigned int end = engine ? 0x2ac : 0x264;
		int n;

		memset(storage, 0xa5, sizeof(storage));
		n = g16_render_pack_sku(sku, 0x400, engine, &a);
		assert(n == (engine ? 0x2c0 : 0x280));
		assert(storage[0] == 0xa5 && sku[n] == 0xa5);
		assert(g16_render_get(sku, 4) == (engine ? 7 : 5));
		assert(g16_render_get(sku + 0x14, 8) == a.command_va + (engine ? 0x80 : 0x40));
		assert(g16_render_get(sku + finish - 4, 4) == 1);
		assert(g16_render_get(sku + finish, 4) == (engine ? 8 : 6));
		assert((int32_t)g16_render_get(sku + finish + restart, 4) + (int)finish == 0);
		assert(g16_render_get(sku + finish + (engine ? 0x98 : 0x78), 8) == a.sku_va + uma);
		assert(!memcmp(sku + uma, a.uma, sizeof(a.uma)));
		assert(g16_render_get(sku + end, 4) == 0x40000002);
		memcpy(before, storage, sizeof(storage));
		assert(g16_render_pack_sku(sku, n - 1, engine, &a) == -ENOSPC);
		assert(!memcmp(storage, before, sizeof(storage)));
	}
	a.context_id = 64;
	assert(g16_render_pack_sku(sku, 0x400, G16_RENDER_TA, &a) == -EINVAL);
}

int main(void)
{
	test_register_bounds();
	test_pages();
	test_sku();
	puts("PASS: render register boundaries, page IDs, SKU control flow and unaligned encoding");
	return 0;
}
