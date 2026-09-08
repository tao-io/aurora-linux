/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_FRAGMENT_H
#define G16_RENDER_FRAGMENT_H
#include "g16_render_pack.h"

/* M4/25G83 generateRegisterListFor3D, 89c162c. See the TA adapter contract. */
#define G16_RENDER_FRAGMENT_DESCRIPTOR_BYTES 0xb69
struct g16_render_fragment_platform {
	u64 accelerator_flags_6a8, pb_slot_word_28, default_d428;
	u32 accelerator_count_4e4;
	u8 accelerator_flag_6b1, accelerator_flag_ed20;
	u8 disable_spill_registers, debug_counters;
};

/* M4/25G83 AGX3DChannelSKU::submitBuffer (894aa94). Firmware 21c6c
 * consumes this state when a pass is partially stored or resumed: +928/+930
 * replace BGO registers 15368/15370, +a14/+a1c replace EOT 15378/15380.
 * Preserve the accompanying Z/S state too, including packed EOT alignment.
 */
static inline int g16_render_fragment_resume_state(u8 *work, size_t capacity,
		const u8 *d, size_t descriptor_bytes)
{
	static const struct { u16 dst, src; u8 bytes; } fields[] = {
		{0x7a0, 0x428, 8}, {0x7b0, 0x430, 8}, {0x7c0, 0x438, 8},
		{0x8f8, 0x580, 8}, {0x900, 0x588, 8},
		{0x928, 0x5b0, 8}, {0x930, 0x5b8, 8},
		{0x938, 0x5c0, 8}, {0x940, 0x5c8, 8},
		{0x948, 0x5e0, 8}, {0x950, 0x640, 8}, {0x958, 0x678, 8},
		{0x960, 0x5e8, 8}, {0x968, 0x5f0, 8}, {0x970, 0x670, 8},
		{0x978, 0x5f8, 8}, {0x980, 0x650, 8}, {0x988, 0x6a0, 8},
		{0x990, 0x600, 8}, {0x998, 0x608, 8}, {0x9a0, 0x698, 8},
		{0x9a8, 0x620, 8}, {0x9b0, 0x638, 8},
		{0x9e0, 0x708, 8}, {0x9f4, 0x6d8, 8}, {0x9fc, 0x6e0, 8},
		{0xa14, 0x6f8, 8}, {0xa1c, 0x700, 8},
		{0xa28, 0x720, 8}, {0xa30, 0x418, 4}, {0xa50, 0x5d8, 8},
	};
	unsigned int i;

	if (!work || !d || capacity < 0xa58 ||
	    descriptor_bytes < G16_RENDER_FRAGMENT_DESCRIPTOR_BYTES)
		return -EINVAL;
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		memcpy(work + fields[i].dst, d + fields[i].src, fields[i].bytes);
	return 0;
}

static inline int g16_render_fragment_registers(struct g16_render_register *out,
		size_t capacity, const u8 *descriptor, size_t descriptor_bytes,
		const struct g16_render_fragment_platform *p, u32 uuid, u64 *mode_out)
{
	u64 tile, v, mode, ta_mode, spill, spill_aux, trace;
	u32 screen, layers;
	unsigned int n = 0;

	if (!out || !descriptor || !p || !mode_out ||
	    descriptor_bytes < G16_RENDER_FRAGMENT_DESCRIPTOR_BYTES)
		return -EINVAL;
#define D8(o) descriptor[o]
#define D32(o) ((u32)g16_render_get(descriptor + (o), 4))
#define D64(o) g16_render_get(descriptor + (o), 8)
#define R(r, f, v) do { \
	if (n == capacity) return -ENOSPC; \
	out[n++] = (struct g16_render_register){(r), (f), (v)}; \
} while (0)
	R(0x1738, 1, 1);
	if (D8(0xa68) & 1) R(0x17e0, 1, 1);
	tile = (D32(0x41c) == 32 ? 0x2000 : 0x1000) |
		(D32(0x420) == 32 ? 0x8000 : 0x4000);
	/* x4 at 89c173c..89c1758 supplies the value omitted by decompilation. */
	R(0x10008, 1, tile | (D32(0x7f8) == 4 ? 2 : D32(0x7f8) == 2 ? 1 : 0));
	R(0x15378, 1, D64(0x6d8));
	R(0x15380, 1, D64(0x6e0));
	R(0x15368, 1, D64(0x580));
	R(0x15370, 1, D64(0x588));
	R(0x15130, 1, D32(0x810));
	R(0x15138, 1, D32(0x814));
	R(0x100a0, 1, !!D64(0x440));
	R(0x15068, 1, D64(0x440) & ~255ULL);
	R(0x15070, 1, D64(0x448) & ~255ULL);
	R(0x16058, 0, D64(0x450) & ~255ULL);
	R(0x10018, 1, D64(0x7e8));
	R(0x100b0, 1, D32(0x800));
	R(0x16030, 0, D32(0x800));
	screen = D32(0x804);
	/* x4 at 89c1c7c is the full 32-bit screen word. */
	R(0x100d8, 1, screen);
	R(0xa300, 1, 0);
	v = D8(0x759) & 1 ? 0xff0004 : 0xff0200;
	if (D8(0xa68) & 1) v = (v & 0x204) | 0x1000000 | (u64)D32(0xa50) << 16;
	R(0x10790, 1, v);
	R(0x16098, 0, D64(0x7e0) & ~63ULL);
	R(0x15108, 1, D64(0x430) & ~3ULL);
	R(0x15100, 1, D64(0x428) & ~3ULL);
	R(0x15020, 1, D64(0x6c0));
	R(0x15210, 1, D64(0x6c8));
	R(0x15048, 1, D64(0x6d0));
	v = D64(0x6b8);
	if (D8(0x759) & 1) v = (v & 0xffffffffffe0ffffULL) | 0x100000;
	R(0x10050, 1, v);
	R(0x15320, 1, D64(0x5d8));
	R(0x15300, 1, D32(0x720));
	R(0x15308, 1, D32(0x724));
	R(0x15310, 1, D64(0x438) & ~15ULL);
	R(0x15318, 1, D64(0x5c0));
	R(0x15348, 1, D64(0x5c8));
	R(0x15350, 1, D64(0x5d0));
	R(0x15328, 1, D64(0x5e0));
	R(0x15330, 1, D64(0x5e8));
	R(0x15338, 1, D64(0x5f8));
	R(0x15340, 1, D64(0x600));
	R(0x15230, 1, D64(0x610));
	R(0x15220, 1, D64(0x618));
	R(0x15238, 1, D64(0x628));
	R(0x15228, 1, D64(0x630));
	R(0x15400, 1, D64(0x640));
	R(0x15420, 1, D64(0x648));
	R(0x15408, 1, D64(0x650));
	R(0x15428, 1, D64(0x658));
	R(0x153c0, 1, D64(0x660));
	R(0x15410, 1, D64(0x678));
	R(0x153c8, 1, D64(0x668));
	R(0x15430, 1, D64(0x680));
	R(0x153d0, 1, D64(0x688));
	R(0x15418, 1, D64(0x6a0));
	R(0x153d8, 1, D64(0x690));
	R(0x15438, 1, D64(0x6a8));
	R(0x16428, 1, D64(0x7d0));
	R(0x16060, 0, D64(0x7d8));
	R(0x16430, 1, (u64)D32(0x824) << 26);
	layers = D32(0x820);
	ta_mode = D8(0x890) == 2 ? 0 : D8(0x890) == 3 ? 0x100 : 0x200;
	mode = ta_mode | (D8(0x891) == 2 ? 0 : D8(0x891) == 3 ? 0x40 : 0x80) |
		(layers > 1) | (u64)(D8(0x7bd) & 1) << 16;
	R(0x10038, 1, mode);
	R(0x16020, 0, 0);
	R(0x16450, 1, 0);
	R(0x15358, 1, 0);
	if (D64(0x770)) R(0xa0e0, 1, D64(0x770) & ~4095ULL);
	v = D8(0x892) & 1 ? 0x8ae0 : 0x8860;
	if (p->accelerator_count_4e4 > 4 || (p->accelerator_flag_6b1 & 1)) v |= 0x1c;
	R(0x100b8, 0, v);
	R(0x16460, 1, D64(0x6b0));
	R(0x16090, 0, D64(0x6b0));
	R(0x101e8, 1, D32(0x9c8) & 0x7f);
	R(0x160a8, 0, (D8(0xb68) & ~p->accelerator_flag_ed20 & 1) ? 0x1000000 : 0);
	v = (D8(0x7c3) & 1 ? 0x7717f : 0x3717f) |
		(p->accelerator_flags_6a8 & 0x100000) |
		(layers > 1 ? 1ULL << 32 : 0) | (tile >> 12) << 40 |
		ta_mode << 28 | (D64(0x440) ? 1ULL << 34 : 0) |
		(u64)(screen & 0x1ff) << 44 | (u64)((screen >> 12) & 0x1ff) << 53;
	R(0x16068, 0, v);
	R(0x1a0a8, 1, D64(0xa00));
	R(0x1a0b0, 1, D64(0xa08));
	R(0x1a078, 1, D64(0xa10));
	R(0x1a080, 1, D64(0xa18));
	R(0x1a0d8, 1, D64(0xa20));
	R(0x1a0e0, 1, D64(0xa28));
	R(0x101c0, 1, (D64(0xa30) & 0xffffffffffe0ffffULL) | D64(0xa38) << 16);
	R(0xd468, 1, D64(0xa40));
	R(0x1a0f8, 1, D32(0xa54) & 0xfffffff8U);
	if (!p->disable_spill_registers) {
		if (!(D64(0xb58) & 0xfffffc00000ULL)) {
			spill = D64(0x768) ? (D64(0x768) << 32) | 0x400020 : 0x6000400020ULL;
			spill_aux = p->default_d428;
		} else {
			spill = D64(0xb50) & 0xffffffffffffULL;
			spill_aux = (D64(0xb50) >> 48) | (D64(0xb58) & 0x3fffff) << 32;
		}
		R(0xa5a8, 1, spill);
		R(0xd428, 1, spill_aux);
	}
	trace = (u64)D32(0x79c) << 32 | uuid;
	R(0x160e0, 0, trace);
	R(0x1498, 1, trace);
	R(0xa340, 1, trace);
	if (p->debug_counters) {
		u32 lo = D32(0x7a0), hi = D32(0x7a4);

		R(0x10048, 1, (lo > 0x3ffe ? 0x3fff : lo) |
			(u64)(hi > 0x3ffe ? 0x3fff : hi) << 16);
		R(0x15078, 1, (lo ? 0 : 0x10) | (hi >= 0x3fff ? 0x20 : 0));
	}
	R(0x1c838, 0, D32(0x830));
	R(0x1ca28, 0, p->pb_slot_word_28 & ~15ULL);
	/* Caller writes mode at command+9d8 and zero at command+9e0. */
	*mode_out = mode;
#undef D8
#undef D32
#undef D64
#undef R
	return n;
}
#endif
