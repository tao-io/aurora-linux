/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_GEOMETRY_H
#define G16_RENDER_GEOMETRY_H
#include "g16_render_pack.h"

/*
 * Single-sample, one-layer path of M4/25G83 halComputeGeometry (89ca2a0).
 * Keep allocation sizes separate from the packed register values. The native
 * NEON shift constants at 712de80/88 are {10,18}/{18,10}.
 */
struct g16_render_geometry {
	u32 pixels, screen, x_blocks, y_blocks, blocks, tiles;
	u32 region_stride, tpc_stride, isp_blocks;
	u64 utile_config, tilemap_bytes, tpc_bytes;
};

static inline int g16_render_geometry_single(struct g16_render_geometry *g,
		u32 width, u32 height, u32 utile_width, u32 utile_height,
		u32 accelerator_count)
{
	u32 tx, ty, bx, by, sx, sy, entries;

	if (!g || !width || !height || width > 16384 || height > 16384 ||
	    (utile_width != 16 && utile_width != 32) ||
	    (utile_height != 16 && utile_height != 32) ||
	    !accelerator_count || accelerator_count > 64)
		return -EINVAL;
	tx = (width + 31) >> 5;
	ty = (height + 31) >> 5;
	bx = (((tx + 3) >> 2) + 3) & ~3U;
	by = (((ty + 3) >> 2) + 3) & ~3U;
	sx = bx << (utile_width == 16);
	sy = by << (utile_height == 16);
	entries = (sx * 4) * (sy * 4);
	*g = (struct g16_render_geometry) {
		.pixels = (width - 1) | (height - 1) << 16,
		.screen = (tx - 1) | (ty - 1) << 12,
		.x_blocks = (bx << 10) | (bx << 18) | bx * 3,
		.y_blocks = (by << 18) | (by << 10) | by * 3,
		.blocks = bx * by, .tiles = tx * ty,
		.region_stride = entries * 5 >> 6,
		.tpc_stride = entries >> 3,
		.isp_blocks = ((sx & 0x3fd) << 16) | (sy & 0x3fd),
		.utile_config = (utile_width == 32 ? 0x2000 : 0x1000) |
				(utile_height == 32 ? 0x8000 : 0x4000),
		.tilemap_bytes = ((u64)entries * 5 + 0xfff) & ~0xfffULL,
		.tpc_bytes = (u64)accelerator_count * entries * 8,
	};
	return 0;
}

/*
 * halLoadPBDesc (89c4338): the 16-byte hardware descriptor is distinct from
 * the firmware's 0xc0-byte manager. All page cursors/counts are 22-bit values.
 * Preserve the native reserved bits when updating an existing descriptor.
 */
static inline int g16_render_pack_pb_hw(u8 *out, size_t bytes,
		u64 page_list_gpu_va, u32 pages, u32 read_cursor, u32 write_cursor,
		u8 wrapped, u8 valid)
{
	u32 w[4], high;
	u64 bias;
	unsigned int i;

	if (!out || bytes < 16 || !page_list_gpu_va ||
	    (page_list_gpu_va & 127) || page_list_gpu_va >= (1ULL << 43) ||
	    !pages || pages > 0x3fffff || read_cursor > 0x3fffff ||
	    write_cursor > 0x3fffff || wrapped > 1 || valid > 1)
		return -EINVAL;
	for (i = 0; i < 4; i++)
		w[i] = g16_render_get(out + 4 * i, 4);
	bias = page_list_gpu_va & (1ULL << 42) ? 0 : 0x7000000000ULL;
	high = ((page_list_gpu_va + bias) >> 8 & 0x70000000) |
		(page_list_gpu_va >> 11 & 0x80000000);
	w[0] = ((page_list_gpu_va >> 4) & 0xfffffff8U) | (w[0] & 6) | valid;
	w[1] = high | (w[1] & 0xfc00000) | pages;
	w[2] = (w[2] & 0xffc00000) | read_cursor;
	w[3] = (w[3] & 0x7fc00000) | write_cursor | (u32)wrapped << 31;
	for (i = 0; i < 4; i++)
		g16_render_put(out + 4 * i, w[i], 4);
	return 0;
}
#endif
