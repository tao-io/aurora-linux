/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_TA_H
#define G16_RENDER_TA_H
#include "g16_render_pack.h"

/*
 * Register producer translated from AGXTAChannelG16::generateRegisterList,
 * 25G83 0xfffffe00089cbcb0. The flat descriptor is an offset-labelled input
 * adapter, not a live macOS object: no embedded CPU pointer is dereferenced.
 * Caller supplies the two PB slot words and platform defaults separately.
 */
#define G16_RENDER_TA_DESCRIPTOR_BYTES 0x1428
struct g16_render_ta_platform {
	u64 accelerator_flags_6a8;
	u64 pb_slot_word_00;
	u64 pb_slot_word_28;
	u64 default_d418;
	u32 accelerator_count_4e4;
	u8 accelerator_flag_6b1;
	u8 disable_spill_registers;
	u8 debug_counters;
};

static inline int g16_render_ta_registers(struct g16_render_register *out,
		size_t capacity, const u8 *descriptor, size_t descriptor_bytes,
		const struct g16_render_ta_platform *p, u32 uuid, u64 *mode_out)
{
	unsigned int n = 0, i;
	u64 v, sign, config, spill, spill_aux, trace;
	u32 layers;

	if (!out || !descriptor || !p || !mode_out ||
	    descriptor_bytes < G16_RENDER_TA_DESCRIPTOR_BYTES)
		return -EINVAL;
#define D8(o) descriptor[o]
#define D16(o) g16_render_get(descriptor + (o), 2)
#define D32(o) ((u32)g16_render_get(descriptor + (o), 4))
#define D64(o) g16_render_get(descriptor + (o), 8)
#define R(r, f, v) do { \
	if (n == capacity) return -ENOSPC; \
	out[n++] = (struct g16_render_register){(r), (f), (v)}; \
} while (0)
	if (!D32(0x1038))
		return -EINVAL;
	R(0x1748, 0, 1);
	if (D8(0x13f0) & 1)
		R(0x17e0, 1, 1);
	R(0x10140, 1, D8(0x890) == 2 ? 0 : D8(0x890) == 3 ? 0x100 : 0x200);
	R(0x1c038, 1, D64(0x1018));
	R(0x1c9c8, 0, D64(0x1018));
	R(0x1c0a0, 1, D64(0x1000));
	sign = p->accelerator_flags_6a8 & (1ULL << 20) ? 0 : 1ULL << 63;
	v = sign | (D64(0x1040) & ~63ULL);
	R(0x1c030, 1, v);
	R(0x1c9c0, 0, v);
	R(0x1c050, 1, D64(0xf88));
	R(0x1c060, 1, D16(0xf90));
	R(0x10148, 1, D64(0x10a8));
	R(0x10138, 1, D64(0x10b0));
	R(0x10110, 1, D64(0xf48));
	R(0x1c9b0, 0, D64(0xf48));
	R(0x10118, 1, D64(0xf50));
	R(0x1c9b8, 0, D64(0xf50));
	R(0x1c958, 0, 1);
	R(0x1c950, 0, (D64(0xf08) & ~31ULL) | (u64)D32(0xf10) << 50);
	R(0x1c930, 0, 0);
	R(0x1c880, 0, D64(0xea8) & ~3ULL);
	R(0x1c078, 1, D64(0x1030));
	R(0x1c9d8, 0, D64(0x1030));
	R(0x10150, 1, D64(0xed8) ? (D64(0xf00) << 8) | 1 : 0);
	for (i = 0; i < 5; i++)
		R(0x1c198 + i * 8, 1, D64(0xed8 + i * 8) & ~255ULL);
	if (D64(0xfd0))
		R(0xa0e8, 1, D64(0xfd0) & ~4095ULL);
	v = D8(0x1138) & 1 ? 0x8ae0 : 0x8860;
	if (p->accelerator_count_4e4 > 4 || (p->accelerator_flag_6b1 & 1))
		v |= 0x1c;
	R(0x1c8f8, 0, v);
	R(0x1c0b0, 1, D32(0x1020));
	R(0x1c850, 0, D32(0x1020));
	R(0x10130, 1, 0x88);
	v = (D8(0xff2) & 1 ? 0x600e : 2) | (D8(0xff8) & 1) |
		((p->accelerator_flags_6a8 >> 7) & 0x200);
	if (p->debug_counters && (D32(0x7a0) || D32(0x7a4) != (u32)-1))
		v |= 0x800;
	R(0x10120, 1, v);
	R(0x10128, 1, D32(0x10b8));
	R(0x101b8, 1, D32(0x10bc));
	R(0x1c068, 1, D32(0x10c0));
	R(0x1c070, 1, D32(0x10c4));
	R(0x1c080, 1, D32(0x10c8));
	R(0x1c0a8, 1, D32(0x1010));
	R(0x10170, 1, 0x100);
	layers = (D32(0x1038) - 1) | 0x8000;
	if (D32(0x1038) > 1)
		layers |= (D8(0xff6) & 1 ? 0x2000 : 0x1000) | 0x4000;
	R(0x10168, 1, layers);
	R(0xa308, 1, 0);
	R(0x1c8e0, 0, ~(u64)0);
	R(0x1c8e8, 0, ~(u64)0);
	config = ((D8(0x1410) == 3 || D8(0x1410) == 4) ? 4 : 0) |
		 ((D8(0x1410) & 0xfe) == 2 ? 2 : 0) | (u64)(D8(0x1411) & 1) << 3;
	/* Disassembly 89ccd04..89ccd50 confirms x4 (missing in decomp call). */
	R(0x1c898, 0, config);
	R(0x101e0, 1, D32(0x1268) & 0x7f);
	R(0x1c9e8, 0, layers & 0x47ff);
	R(0x1a098, 1, D64(0x1388));
	R(0x1a0a0, 1, D64(0x1390));
	R(0x1a068, 1, D64(0x1398));
	R(0x1a070, 1, D64(0x13a0));
	R(0x1a0c8, 1, D64(0x13a8));
	R(0x1a0d0, 1, D64(0x13b0));
	R(0x101c8, 1, (D64(0x13b8) & 0xffffffffffe0ffffULL) | D64(0x13c0) << 16);
	R(0xd470, 1, D64(0x13c8));
	R(0x1a0f0, 1, D32(0x13dc) & 0xfffffff8U);
	R(0x10798, 1, D8(0x13f0) & 1 ? ((u64)D32(0x13d8) << 16) | 0x1000000 : 0xff0000);
	if (D8(0x1408) & 1)
		R(0x1ca48, 0, (D64(0x13f8) & ~1023ULL) | D64(0x1400) << 48);
	R(0x1c830, 0, D32(0x10d0));
	R(0x1ca30, 0, p->pb_slot_word_28 & ~15ULL);
	R(0x16c38, 1, p->pb_slot_word_28 & ~15ULL);
	v = p->pb_slot_word_00;
	R(0x1c910, 0, ((v >> 3) & 0x8000000000ULL) |
		 (((v & 0x40000000000ULL ? 0 : 0x7000000000ULL) + v) & 0x7ffffffffeULL) | 1);
	if (!p->disable_spill_registers) {
		if (!(D64(0x1378) & 0xfffffc00000ULL)) {
			spill = D64(0xfe0) ? (D64(0xfe0) << 32) | 0x400020 : 0x6000400020ULL;
			spill_aux = p->default_d418;
		} else {
			spill = D64(0x1370) & 0xffffffffffffULL;
			spill_aux = (D64(0x1370) >> 48) | ((D64(0x1378) & 0x3fffff) << 32);
		}
		R(0xa5a0, 1, spill);
		R(0xd418, 1, spill_aux);
	}
	trace = ((u64)D32(0x79c) << 32) | uuid;
	R(0x1ca10, 0, trace);
	R(0x14a0, 1, trace);
	R(0xa348, 1, trace);
	if (p->debug_counters) {
		u32 lo = D32(0x7a0), hi = D32(0x7a4);

		if (lo > 0x3ffe) lo = 0x3fff;
		if (hi > 0x3ffe) hi = 0x3fff;
		R(0x10278, 1, lo | (u64)hi << 16);
		R(0x10048, 1, lo | (u64)hi << 16);
	}
	/* Deliberately no global debug register overrides. */
	*mode_out = config;
#undef D8
#undef D16
#undef D32
#undef D64
#undef R
	return n;
}
#endif
