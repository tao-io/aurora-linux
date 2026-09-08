// SPDX-License-Identifier: GPL-2.0-only
/* Host-side ABI and rejection tests; never access the GPU. */
#include <assert.h>
#include <stdio.h>
#include "g16_render_pack.h"
#include "g16_render_ta.h"
#include "g16_render_fragment.h"
#include "g16_render_geometry.h"
#include "g16_render_stream.h"

static void test_init_pb(void)
{
	u8 storage[0x22], before[sizeof(storage)], *command = storage + 1;
	const u8 expected[0x20] = {
		6, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		32, 0, 0, 0, 0, 0x90, 0, 0, 0x30, 0xfc, 0xff, 0xff,
		0, 1, 0, 0,
	};

	memset(storage, 0xa5, sizeof(storage));
	assert(!g16_render_pack_init_pb(command, 0x20, 6, 0, 0, 32,
				       0xfffffc3000009000ULL, 0x100));
	assert(!memcmp(command, expected, sizeof(expected)));
	assert(storage[0] == 0xa5 && storage[0x21] == 0xa5);
	memcpy(before, storage, sizeof(storage));
	assert(g16_render_pack_init_pb(command, 0x1f, 6, 0, 0, 32,
		0xfffffc3000009000ULL, 0x100) == -EINVAL);
	assert(!memcmp(before, storage, sizeof(storage)));
	assert(g16_render_pack_init_pb(command, 0x20, 64, 0, 0, 32,
		0xfffffc3000009000ULL, 0x100) == -EINVAL);
	assert(!memcmp(before, storage, sizeof(storage)));
}

static void test_geometry(void)
{
	struct g16_render_geometry g, before;
	u8 hw[16] = {0}, saved[16];

	assert(!g16_render_geometry_single(&g, 4, 4, 32, 32, 1));
	assert(g.pixels == 0x30003 && !g.screen && g.tiles == 1);
	assert(g.x_blocks == 0x10100c && g.y_blocks == 0x10100c);
	assert(g.region_stride == 20 && g.tpc_stride == 32);
	assert(g.tilemap_bytes == 4096 && g.tpc_bytes == 2048);
	/* J713 panel: 80x50 tiles, padded to 20x16 groups of 4x4. */
	assert(!g16_render_geometry_single(&g, 2560, 1600, 32, 32, 1));
	assert(g.tiles == 4000 && g.blocks == 320);
	assert(g.region_stride == 400 && g.tpc_stride == 640);
	assert(g.tilemap_bytes == 28672 && g.tpc_bytes == 40960);
	before = g;
	assert(g16_render_geometry_single(&g, 0, 4, 32, 32, 1) == -EINVAL);
	assert(!memcmp(&g, &before, sizeof(g)));
	assert(g16_render_geometry_single(&g, 4, 16385, 32, 32, 1) == -EINVAL);
	assert(g16_render_geometry_single(&g, 4, 4, 8, 32, 1) == -EINVAL);
	assert(!g16_render_pack_pb_hw(hw, sizeof(hw), 0x10000a000, 128, 0, 127, 0, 1));
	assert(g16_render_get(hw, 4) == 0x10000a01);
	assert(g16_render_get(hw + 4, 4) == 0x70000080);
	assert(!g16_render_get(hw + 8, 4));
	assert(g16_render_get(hw + 12, 4) == 127);
	/* Native first-use ring is zero; bit 42 must survive the VA encoding. */
	assert(!g16_render_pack_pb_hw(hw, sizeof(hw), 0x4300000a000ULL,
				      128, 0, 0, 0, 0));
	assert(g16_render_get(hw, 4) == 0xa00);
	assert(g16_render_get(hw + 4, 4) == 0xb0000080);
	assert(!g16_render_get(hw + 8, 8));
	memcpy(saved, hw, sizeof(hw));
	assert(g16_render_pack_pb_hw(hw, sizeof(hw), 0x10000a001, 128, 0, 127, 0, 1) == -EINVAL);
	assert(!memcmp(saved, hw, sizeof(hw)));
}

static u64 reg_value(const struct g16_render_register *r, int n, u32 reg)
{
	int i;

	for (i = 0; i < n; i++)
		if (r[i].reg == reg)
			return r[i].value;
	assert(!"missing register");
	return 0;
}

static void test_ta_registers(void)
{
	u8 descriptor[G16_RENDER_TA_DESCRIPTOR_BYTES] = {0};
	struct g16_render_register regs[128];
	struct g16_render_ta_platform p = {.accelerator_count_4e4 = 10};
	u64 mode;
	int n;

	assert(g16_render_ta_registers(regs, 128, descriptor, sizeof(descriptor),
				       &p, 0, &mode) == -EINVAL);
	g16_render_put(descriptor + 0x1038, 1, 4);
	g16_render_put(descriptor + 0xf48, 0x1000018000, 8);
	g16_render_put(descriptor + 0x1040, 0x100001683f, 8);
	descriptor[0x1410] = 3;
	descriptor[0x1411] = 1;
	p.pb_slot_word_28 = 0x600012345f;
	n = g16_render_ta_registers(regs, 128, descriptor, sizeof(descriptor),
				    &p, 0x1234, &mode);
	assert(n > 60 && n <= 125);
	assert(mode == 14 && reg_value(regs, n, 0x1c898) == mode);
	assert(reg_value(regs, n, 0x10110) == 0x1000018000);
	assert(reg_value(regs, n, 0x1c9b0) == 0x1000018000);
	assert(reg_value(regs, n, 0x1c030) == 0x8000001000016800ULL);
	assert(reg_value(regs, n, 0x1c9c0) == reg_value(regs, n, 0x1c030));
	assert(reg_value(regs, n, 0x1c8f8) == 0x887c);
	assert(reg_value(regs, n, 0x1ca30) == 0x6000123450);
	assert(reg_value(regs, n, 0x16c38) == 0x6000123450);
	assert(reg_value(regs, n, 0x1ca10) == 0x1234);
	assert(g16_render_ta_registers(regs, n - 1, descriptor, sizeof(descriptor),
				       &p, 0, &mode) == -ENOSPC);
}

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

static void test_stream(void)
{
	/* Qualified 25G83 direct-draw stream, independent of this encoder. */
	static const u8 reference[120] = {
		0x2e, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x52, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
		0x00, 0x07, 0x00, 0x00, 0x00, 0x80, 0x05, 0x00, 0x00, 0x05, 0x00, 0x00,
		0x1c, 0x80, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x30, 0x80, 0x05, 0x00,
		0x00, 0x05, 0x00, 0x00, 0x4c, 0x80, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00,
		0x00, 0x89, 0x06, 0x00, 0x00, 0x03, 0x00, 0x00, 0x60, 0x80, 0x05, 0x00,
		0x00, 0x02, 0x00, 0x00, 0x6c, 0x80, 0x05, 0x00, 0x00, 0x02, 0x00, 0x00,
		0x00, 0x80, 0x04, 0x00, 0x00, 0x06, 0xc4, 0x61, 0x03, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
	};
	u8 storage[122], before[122];

	memset(storage, 0xa5, sizeof(storage));
	assert(g16_render_pack_stream(storage + 1, 120, 0x10000148000ULL, 3, 0) == 120);
	assert(!memcmp(storage + 1, reference, 120));
	assert(storage[0] == 0xa5 && storage[121] == 0xa5);
	memcpy(before, storage, sizeof(storage));
	assert(g16_render_pack_stream(storage + 1, 119, 0x10000148000ULL, 3, 0) == -EINVAL);
	assert(g16_render_pack_stream(storage + 1, 120, 0x10000148001ULL, 3, 0) == -EINVAL);
	assert(g16_render_pack_stream(storage + 1, 120, 0x10000148000ULL, 4, 3) == -EINVAL);
	assert(g16_render_pack_stream(storage + 1, 120, 0x10000148000ULL, 0, 0) == -EINVAL);
	assert(!memcmp(storage, before, sizeof(storage)));
	assert(g16_render_pack_stream(storage + 1, 120, 0x1000014c000ULL, 3, 3) == 120);
	assert(g16_render_get(storage + 1 + 0xc, 4) == 0x5300);
	assert(g16_render_get(storage + 1 + 0x70, 4) == 3);
}

int main(void)
{
	u8 descriptor[G16_RENDER_FRAGMENT_DESCRIPTOR_BYTES] = {0};
	struct g16_render_fragment_platform p = {0};
	struct g16_render_register regs[128];
	u64 mode;
	int n;

	test_stream();
	test_init_pb();
	test_geometry();
	test_register_bounds();
	test_pages();
	test_sku();
	test_ta_registers();
	n = g16_render_fragment_registers(regs, 128, descriptor, sizeof(descriptor), &p, 0, &mode);
	assert(n > 80 && n <= 126);
	assert(reg_value(regs, n, 0x10008) == 0x5000);
	assert(reg_value(regs, n, 0x10038) == mode && mode == 0x280);
	assert(g16_render_fragment_registers(regs, n - 1, descriptor, sizeof(descriptor), &p, 0, &mode) == -ENOSPC);
	assert(g16_render_fragment_registers(regs, 128, descriptor, sizeof(descriptor) - 1, &p, 0, &mode) == -EINVAL);
	puts("PASS: direct draw stream, register bounds, page IDs, SKU control flow, geometry, InitPB, PB descriptor and both stage producers");
	return 0;
}
