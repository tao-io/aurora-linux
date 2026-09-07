/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_PACK_H
#define G16_RENDER_PACK_H

/*
 * M4 / 25G83 only. Byte writers deliberately support unaligned firmware fields.
 * This is a packet builder, not an allocator or a submission/completion API.
 * See README.md for producer and firmware-consumer evidence.
 */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

enum g16_render_engine { G16_RENDER_TA, G16_RENDER_FRAGMENT };
#define G16_RENDER_REG_BYTES 12
#define G16_RENDER_APERTURES 0x600
#define G16_RENDER_REGLIST_HEADER 0x700
#define G16_RENDER_TA_COMMAND_BYTES 0x940
#define G16_RENDER_FRAGMENT_COMMAND_BYTES 0xca0
#define G16_RENDER_TA_START_BYTES 0x1cc
#define G16_RENDER_FRAGMENT_START_BYTES 0x1ec
#define G16_RENDER_TA_FINISH_BYTES 0x94
#define G16_RENDER_FRAGMENT_FINISH_BYTES 0xbc
#define G16_RENDER_PB_SLOT_BYTES 0x80
#define G16_RENDER_PB_MANAGER_BYTES 0xc0

static inline void g16_render_put(u8 *p, u64 v, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		p[i] = v >> (8 * i);
}

static inline u64 g16_render_get(const u8 *p, unsigned int n)
{
	u64 v = 0;
	unsigned int i;

	for (i = 0; i < n; i++)
		v |= (u64)p[i] << (8 * i);
	return v;
}

struct g16_render_register {
	u32 reg;
	u8 flag;
	u64 value;
};

/* Leaves firmware's stage-specific suffix and attachment aperture untouched. */
static inline int g16_render_pack_registers(u8 *command, size_t bytes,
		enum g16_render_engine engine, u64 command_low_va,
		const struct g16_render_register *regs, size_t count)
{
	unsigned int base, suffix, i;
	u8 *header;

	if (engine != G16_RENDER_TA && engine != G16_RENDER_FRAGMENT)
		return -EINVAL;
	base = engine == G16_RENDER_TA ? 0x40 : 0x80;
	suffix = engine == G16_RENDER_TA ? 3 : 2;
	if (!command || !regs || !count || bytes < base + 0x70c)
		return -EINVAL;
	if (!command_low_va || (command_low_va & 7) ||
	    command_low_va >= (1ULL << 42) - base - 0x70c)
		return -ERANGE;
	if (count > G16_RENDER_APERTURES / G16_RENDER_REG_BYTES - suffix)
		return -E2BIG;
	for (i = 0; i < count; i++)
		if ((regs[i].reg & ~0x3fff8U) || regs[i].flag > 1)
			return -EINVAL;

	/* Do not clear the block: the caller may already have populated apertures. */
	for (i = 0; i < count; i++) {
		u8 *entry = command + base + i * G16_RENDER_REG_BYTES;

		g16_render_put(entry, regs[i].reg | regs[i].flag, 4);
		g16_render_put(entry + 4, regs[i].value, 8);
	}
	memset(command + base + count * G16_RENDER_REG_BYTES, 0,
	       suffix * G16_RENDER_REG_BYTES);
	header = command + base + G16_RENDER_REGLIST_HEADER;
	g16_render_put(header, command_low_va + base, 8);
	g16_render_put(header + 8, count, 2);
	g16_render_put(header + 10, count * G16_RENDER_REG_BYTES, 2);
	return 0;
}

/*
 * AGXParameterManagementVirtual::prepareParameterBufferBlock (89e4010) and
 * buildPageList (89e0bd8): 25-bit 32 KiB virtual page IDs, NOT Linux PFNs.
 * Each supplied address must have both constituent 16 KiB UAT leaves backed.
 */
static inline int g16_render_pack_page_list(u8 *list, size_t bytes,
		const u64 *gpu_pages, size_t count)
{
	size_t i;

	if (!list || !gpu_pages || !count || count > bytes / 4)
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (!gpu_pages[i] || (gpu_pages[i] & 0x7fff) ||
		    gpu_pages[i] >= (1ULL << 40))
			return -ERANGE;
	for (i = 0; i < count; i++)
		g16_render_put(list + i * 4, gpu_pages[i] >> 15, 4);
	return 0;
}

struct g16_render_sku_args {
	/* Firmware pointers: caller must map and initialize all pointed-to objects. */
	u64 command_va;
	u64 sku_va;
	u64 queue_va;
	u64 slot_array_va; /* Already points past TA +4 / fragment +8 array header. */
	u64 pb_slot_va;    /* 0x80-byte per-pass state, not the 0xc0-byte HW manager. */
	u64 buffer_info_va;
	u32 context_id;
	u32 context_control;
	u32 submission_slot;
	u32 pb_slot;
	u32 pass_id;
	u32 event_control;
	u32 uuid;
	u32 stamp;
	u64 stamp_va;
	u32 stamp_slot;
	u32 fragment_increment;
	u8 mode;
	/* Pool binding prefix; the following 12 bytes are stage-owned fields. */
	u8 uma[0x30];
};

struct g16_render_job_args {
	struct g16_render_sku_args sku;
	u64 command_counter;
	u64 notifier_va;
	u64 shared_stamp_va;
	u64 pb_aux_va;
	u32 event_sequence;
	u8 context_generation;
};

/* Initialize the envelope; render-state and register builders fill its body. */
static inline int g16_render_pack_job(u8 *command, size_t bytes,
		enum g16_render_engine engine, const struct g16_render_job_args *j,
		u32 sku_bytes)
{
	unsigned int size, meta;
	const struct g16_render_sku_args *a;

	if (engine != G16_RENDER_TA && engine != G16_RENDER_FRAGMENT)
		return -EINVAL;
	size = engine == G16_RENDER_TA ? G16_RENDER_TA_COMMAND_BYTES :
		G16_RENDER_FRAGMENT_COMMAND_BYTES;
	meta = engine == G16_RENDER_TA ? 0x878 : 0xbc0;
	if (!command || !j || bytes < size || !j->notifier_va ||
	    !j->shared_stamp_va || !j->pb_aux_va || !sku_bytes || (sku_bytes & 63))
		return -EINVAL;
	a = &j->sku;
	if (a->context_id >= 64 || !a->sku_va || !a->stamp_va ||
	    !a->buffer_info_va || !a->pb_slot_va)
		return -EINVAL;
	memset(command, 0, size);
	g16_render_put(command, engine == G16_RENDER_TA ? 0 : 1, 4);
	g16_render_put(command + 4, j->command_counter, 8);
	g16_render_put(command + 0xc, a->context_id, 4);
	g16_render_put(command + meta, j->shared_stamp_va, 8);
	g16_render_put(command + meta + 8, a->stamp_va, 8);
	g16_render_put(command + meta + 0x10, a->stamp, 4);
	g16_render_put(command + meta + 0x14, a->stamp_slot, 4);
	g16_render_put(command + meta + 0x18, a->event_control, 4);
	g16_render_put(command + meta + 0x20, a->uuid, 4);
	g16_render_put(command + meta + 0x24, j->event_sequence, 4);
	if (engine == G16_RENDER_TA) {
		g16_render_put(command + 0x10, j->notifier_va, 8);
		g16_render_put(command + 0x18, a->pb_slot, 4);
		g16_render_put(command + 0x20, a->buffer_info_va, 8);
		g16_render_put(command + 0x28, a->pb_slot_va, 8);
		g16_render_put(command + 0x30, j->pb_aux_va, 8);
		g16_render_put(command + 0x770, a->sku_va, 8);
		g16_render_put(command + 0x778, sku_bytes, 4);
		g16_render_put(command + 0x8a0, a->pass_id, 4);
		g16_render_put(command + 0x8a4, a->mode == 2, 4);
		command[0x917] = j->context_generation;
	} else {
		g16_render_put(command + 0x14, a->sku_va, 8);
		g16_render_put(command + 0x1c, sku_bytes, 4);
		g16_render_put(command + 0x20, j->notifier_va, 8);
		g16_render_put(command + 0x28, a->buffer_info_va, 8);
		g16_render_put(command + 0x30, a->pb_slot_va, 8);
		g16_render_put(command + 0x38, j->pb_aux_va, 8);
		g16_render_put(command + 0xbe8, a->pass_id, 4);
		g16_render_put(command + 0xbec, a->mode == 2, 4);
		command[0xc5f] = j->context_generation;
		command[0xc84] = a->mode;
	}
	return 0;
}

/*
 * Build the simple, no-attachment/no-timestamp start/WFI/finish sequence.
 * Start/finish refer to caller-populated command state. No guessed hardware
 * register values, buffer pointers or pool descriptors are synthesized here.
 * This subset omits optional debug, counter and second-fragment-pass records.
 */
static inline int g16_render_pack_sku(u8 *sku, size_t capacity,
		enum g16_render_engine engine, const struct g16_render_sku_args *a)
{
	unsigned int start, finish, total, uma;
	u8 *f;

	if (engine != G16_RENDER_TA && engine != G16_RENDER_FRAGMENT)
		return -EINVAL;
	if (!sku || !a || !a->command_va || !a->sku_va || !a->queue_va ||
	    !a->slot_array_va || !a->pb_slot_va || !a->buffer_info_va ||
	    !a->stamp_va || a->context_id >= 64 || a->submission_slot >= 64)
		return -EINVAL;
	start = engine == G16_RENDER_TA ? 0x1cc : 0x1ec;
	finish = engine == G16_RENDER_TA ? 0x94 : 0xbc;
	uma = engine == G16_RENDER_TA ? 0x190 : 0x1b0;
	total = (start + 4 + finish + 4 + 63) & ~63U;
	if (capacity < total)
		return -ENOSPC;
	if (a->command_va > ~(u64)0 - G16_RENDER_FRAGMENT_COMMAND_BYTES ||
	    a->sku_va > ~(u64)0 - total || a->pb_slot_va > ~(u64)0 - 0x80)
		return -ERANGE;
	memset(sku, 0, total);
	g16_render_put(sku, engine == G16_RENDER_TA ? 5 : 7, 4);
	g16_render_put(sku + 0x14, a->command_va +
		       (engine == G16_RENDER_TA ? 0x40 : 0x80), 8);
	memcpy(sku + uma, a->uma, sizeof(a->uma));
	f = sku + start + 4;
	g16_render_put(f, engine == G16_RENDER_TA ? 6 : 8, 4);

	if (engine == G16_RENDER_TA) {
		g16_render_put(sku + 0x1c, a->buffer_info_va, 8);
		g16_render_put(sku + 0x24, a->pb_slot_va, 8);
		g16_render_put(sku + 0x2c, a->slot_array_va, 8);
		g16_render_put(sku + 0x34, a->queue_va, 8);
		g16_render_put(sku + 0x3c, a->command_va + 0x85c, 8);
		g16_render_put(sku + 0x44, a->context_id, 4);
		g16_render_put(sku + 0x48, a->context_control, 4);
		g16_render_put(sku + 0x4c, a->submission_slot, 4);
		g16_render_put(sku + 0x50, a->pb_slot, 4);
		g16_render_put(sku + 0x58, a->pass_id, 4);
		g16_render_put(sku + 0x5c, a->event_control, 4);
		g16_render_put(sku + 0x64, a->command_va + 0x784, 8);
		g16_render_put(sku + 0x6c, a->command_va + 0x8a8, 8);
		g16_render_put(sku + 0x7c, a->uuid, 4);
		g16_render_put(sku + 0x1c0, a->stamp_slot, 4);
		g16_render_put(sku + start, 1, 4); /* encodeTAFinishWFI */

		g16_render_put(f + 0x04, a->pb_slot_va, 8);
		g16_render_put(f + 0x0c, a->buffer_info_va, 8);
		g16_render_put(f + 0x14, a->slot_array_va, 8);
		g16_render_put(f + 0x1c, a->queue_va, 8);
		g16_render_put(f + 0x24, a->command_va + 0x85c, 8);
		g16_render_put(f + 0x2c, a->context_id, 4);
		g16_render_put(f + 0x34, a->command_va + 0x784, 8);
		g16_render_put(f + 0x40, a->uuid, 4);
		g16_render_put(f + 0x48, a->stamp_va, 8);
		g16_render_put(f + 0x50, a->stamp, 4);
		g16_render_put(f + 0x78, a->sku_va + uma, 8);
		g16_render_put(f + 0x80, (u32)-(start + 4), 4);
		g16_render_put(f + 0x85, a->command_va + 0x918, 8);
		f[0x8d] = a->mode;
	} else {
		g16_render_put(sku + 0x1c, a->pb_slot_va, 8);
		g16_render_put(sku + 0x24, a->slot_array_va, 8);
		g16_render_put(sku + 0x2c, a->command_va + 0xb70, 8);
		g16_render_put(sku + 0x34, a->pb_slot_va + 0x54, 8);
		g16_render_put(sku + 0x3c, a->command_va + 0xbb0, 8);
		g16_render_put(sku + 0x44, a->command_va + 0xbb4, 8);
		g16_render_put(sku + 0x4c, a->queue_va, 8);
		g16_render_put(sku + 0x54, a->command_va, 8);
		g16_render_put(sku + 0x5c, a->context_id, 4);
		g16_render_put(sku + 0x60, a->context_control, 4);
		g16_render_put(sku + 0x64, a->submission_slot, 4);
		g16_render_put(sku + 0x68, a->pb_slot, 4);
		g16_render_put(sku + 0x70, a->pass_id, 4);
		g16_render_put(sku + 0x74, a->event_control, 4);
		g16_render_put(sku + 0x7c, a->command_va + 0xa58, 8);
		g16_render_put(sku + 0x84, a->command_va + 0xbf0, 8);
		g16_render_put(sku + 0xa0, a->uuid, 4);
		g16_render_put(sku + 0x1e0, a->stamp_slot, 4);
		sku[0x1e4] = a->mode;
		/* AGX3DChannelG16::encodePixelEndRenderWFI (89bedd0). */
		g16_render_put(sku + start, 1, 4);

		g16_render_put(f + 0x04, a->uuid, 4);
		g16_render_put(f + 0x0c, a->stamp_va, 8);
		g16_render_put(f + 0x14, a->stamp, 4);
		g16_render_put(f + 0x1c, a->pb_slot_va, 8);
		g16_render_put(f + 0x24, a->buffer_info_va, 8);
		g16_render_put(f + 0x2c, 1, 4);
		g16_render_put(f + 0x30, a->slot_array_va, 8);
		g16_render_put(f + 0x38, a->command_va + 0xbb0, 8);
		g16_render_put(f + 0x40, a->command_va + 0xbb4, 8);
		g16_render_put(f + 0x48, a->command_va + 0xb70, 8);
		g16_render_put(f + 0x50, a->queue_va, 8);
		g16_render_put(f + 0x58, a->command_va, 8);
		g16_render_put(f + 0x60, a->context_id, 4);
		g16_render_put(f + 0x64, a->command_va + 0xa58, 8);
		g16_render_put(f + 0x98, a->sku_va + uma, 8);
		g16_render_put(f + 0xa0, (u32)-(start + 4), 4);
		g16_render_put(f + 0xa5, a->command_va + 0xc60, 8);
		g16_render_put(f + 0xad, a->fragment_increment, 4);
		f[0xb1] = a->mode;
		g16_render_put(f + 0xb2, a->stamp_slot, 4);
	}
	g16_render_put(f + finish, 0x40000002, 4); /* finishEncode */
	return total;
}

struct g16_render_pb_manager_args {
	u64 page_list_fw_va;
	u64 page_list_gpu_va;
	u64 block_list_va;
	u64 block_ring_va;
	u64 counter_va;
	u64 discard_va;
	u32 page_list_bytes;
	u32 page_count;
	u32 block_capacity;
	u32 block_wptr;
	u32 block_rptr;
	u32 max_pages;
	u32 min_pages;
	u32 id;
};

/* resetHWParamBufferState + buildPageList, fixed 128 KiB backing blocks. */
static inline int g16_render_pack_pb_manager(u8 *state, size_t bytes,
		const struct g16_render_pb_manager_args *a)
{
	if (!state || !a || bytes < G16_RENDER_PB_MANAGER_BYTES ||
	    !a->page_list_fw_va || !a->page_list_gpu_va ||
	    !a->block_list_va || !a->block_ring_va || !a->counter_va ||
	    !a->block_capacity || !a->page_count || (a->page_count & 3) ||
	    a->page_count > a->page_list_bytes / 4 ||
	    a->page_count / 4 >= a->block_capacity ||
	    a->block_wptr >= a->block_capacity || a->block_rptr >= a->block_capacity ||
	    a->min_pages > a->max_pages || a->max_pages > a->page_list_bytes / 4)
		return -EINVAL;
	memset(state, 0, G16_RENDER_PB_MANAGER_BYTES);
	g16_render_put(state + 0x10, a->id, 4);
	g16_render_put(state + 0x20, a->page_list_fw_va, 8);
	g16_render_put(state + 0x28, a->page_list_gpu_va, 8);
	g16_render_put(state + 0x30, a->page_list_bytes, 4);
	g16_render_put(state + 0x34, a->page_count, 4);
	g16_render_put(state + 0x38, a->block_capacity, 4);
	g16_render_put(state + 0x3c, a->block_wptr, 4);
	g16_render_put(state + 0x40, a->block_rptr, 4);
	g16_render_put(state + 0x44, a->block_list_va, 8);
	g16_render_put(state + 0x4c, a->block_ring_va, 8);
	g16_render_put(state + 0x54, a->page_count - 1, 4);
	g16_render_put(state + 0x58, 0x20000, 4);
	g16_render_put(state + 0x64, a->counter_va, 8);
	g16_render_put(state + 0x7c, a->max_pages, 4);
	g16_render_put(state + 0x80, a->min_pages, 4);
	g16_render_put(state + 0x84, a->discard_va, 8);
	return 0;
}
#endif
