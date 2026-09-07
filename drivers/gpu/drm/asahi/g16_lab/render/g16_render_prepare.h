/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_PREPARE_H
#define G16_RENDER_PREPARE_H

#include "g16_render_pack.h"
#include "g16_render_ta.h"
#include "g16_render_fragment.h"
#include "g16_render_geometry.h"
#include "g16_render_uat.h"
#include "g16_vertex_marker.h"
#include "g16_generated_graphics.h"
#include "g16_generated_bindings.h"
#include "g16_render_stream.h"

#define G16_RENDER_RESOURCE_COUNT 34
#define G16_RENDER_GRAPH_BYTES 0x80000
#define G16_RENDER_POOL_BYTES 0x400000
#define G16_RENDER_GRAPH_VA 0xfffffc3000000000ULL
#define G16_RENDER_POOL_VA 0x6000000000ULL
#define G16_RENDER_COMPACT_BASE 0x1000000000ULL
#define G16_RENDER_QUEUE(i) (0x10000 + (i) * 0x4000)
#define G16_RENDER_PIPE_RING(i) (0x18000 + (i) * 0x2000)
#define G16_RENDER_QUEUE_STATE(i) (0x1c000 + (i) * 0x400)
#define G16_RENDER_QUEUE_RING(i) (G16_RENDER_QUEUE_STATE(i) + 0x80)
#define G16_RENDER_PIPE_STATE(i) (G16_RENDER_QUEUE_STATE(i) + 0x100)
#define G16_RENDER_QUEUE_CONTEXT(i) (G16_RENDER_QUEUE_STATE(i) + 0x140)
#define G16_RENDER_NOTIFIER_LIST(i) (G16_RENDER_QUEUE_STATE(i) + 0x180)
#define G16_RENDER_BARRIER(i) (G16_RENDER_QUEUE_STATE(i) + 0x200)
#define G16_RENDER_SLOT_ARRAY(i) (0x1d000 + (i) * 0x1000)
#define G16_RENDER_PB_HW 0x20000
#define G16_RENDER_INIT_PB 0x8800
#define G16_RENDER_RESOURCE_FILE "g16/render-reference.bin"

struct g16_render_resource {
	u64 va;
	u32 size;
	void *data;
	struct debugfs_blob_wrapper blob;
};

static struct g16_render_resource g16_render_resources[G16_RENDER_RESOURCE_COUNT];
static u8 *g16_render_graph;
static void *g16_render_pool;
static struct dentry *g16_render_graph_file;
static struct debugfs_blob_wrapper g16_render_graph_blob;
static struct dentry *g16_render_output_dir;
static struct debugfs_blob_wrapper g16_render_pool_blob;
static struct debugfs_blob_wrapper g16_render_frames[8];
static struct g16_render_uat g16_render_user_uat;
static struct g16_render_uat g16_render_fw_uat;

static u64 g16_render_target_va(void)
{
	return render_size == 4 ? 0x10000058000ULL : 0x10001000000ULL;
}

static u32 g16_render_target_offset(void)
{
	return render_size == 4 ? 0x100 : 0x4000;
}

static u8 *g16_render_vertices(struct g16_render_resource *r)
{
	u64 address = render_bindings ? 0x1000014d100ULL : 0x10000090000ULL;

	if (address < r->va || address - r->va > r->size ||
	    r->size - (address - r->va) < 24)
		return NULL;
	return (u8 *)r->data + address - r->va;
}

static void g16_render_release(void)
{
	unsigned int i;

	debugfs_remove_recursive(g16_render_output_dir);
	g16_render_output_dir = NULL;
	for (i = 0; i < ARRAY_SIZE(g16_render_frames); i++) {
		vfree(g16_render_frames[i].data);
		memset(&g16_render_frames[i], 0, sizeof(g16_render_frames[i]));
	}
	debugfs_remove(g16_render_graph_file);
	g16_render_graph_file = NULL;
	g16_render_graph_blob.data = NULL;
	g16_render_graph_blob.size = 0;
	g16_render_uat_free(&g16_render_user_uat);
	g16_render_uat_free(&g16_render_fw_uat);
	vfree(g16_render_graph);
	g16_render_graph = NULL;
	vfree(g16_render_pool);
	g16_render_pool = NULL;
	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
		vfree(g16_render_resources[i].data);
		memset(&g16_render_resources[i], 0, sizeof(g16_render_resources[i]));
	}
}

static u64 g16_render_resource_flags(const struct g16_render_resource *r)
{
	/* dPM writes the compact-range scratch; dVDM requires the captured AP2
	 * executable tuple for its command stream. These are distinct clients. */
	if (r->va == 0x100000a8000ULL)
		return J713_UAT_USER_CAPTURE_GPU_DATA_SHARED_FLAGS;
	/* First 3D dispatch reached the EOT image write at target+100 and
	 * reported write-permission fault 0x1948c0407 on the AP2 leaf.
	 */
	return (r->va == 0x1000000000ULL || r->va == g16_render_target_va()) ?
		J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS :
		J713_UAT_USER_CAPTURE_GPU_EXEC_SHARED_FLAGS;
}

static int g16_render_prepare_uat(void)
{
	const u64 mask = (1ULL << 42) - 1;
	unsigned int i;
	int ret;

	/*
	 * The dPM engine faults on writes to AP2 leaves (J713 fault 0x1858d4207
	 * at resource 6, VA 0x1000000000). Use RW/NX for that scratch and owned
	 * data. Keep AP2 executable for command resources: removing AP2 caused
	 * dVDM no-access fault 0x1890d4e19 at VA 0x1000018180.
	 */
	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
		struct g16_render_resource *r = &g16_render_resources[i];

		ret = g16_render_uat_map(&g16_render_user_uat, r->va, r->data,
					r->size, g16_render_resource_flags(r));
		if (ret)
			return ret;
	}
	ret = g16_render_uat_map(&g16_render_user_uat, G16_RENDER_POOL_VA,
		g16_render_pool, G16_RENDER_POOL_BYTES, J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS);
	if (ret)
		return ret;
	ret = g16_render_uat_map(&g16_render_user_uat, G16_RENDER_GRAPH_VA & mask,
		g16_render_graph, G16_RENDER_GRAPH_BYTES, J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS);
	if (ret)
		return ret;
	ret = g16_render_uat_map(&g16_render_fw_uat, G16_RENDER_GRAPH_VA & mask,
		g16_render_graph, G16_RENDER_GRAPH_BYTES, J713_UAT_FW_SHARED_FLAGS);
	if (ret)
		return ret;
	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
		struct g16_render_resource *r = &g16_render_resources[i];

		ret = g16_render_uat_verify(&g16_render_user_uat, r->va, r->data,
					r->size, g16_render_resource_flags(r));
		if (ret)
			return ret;
	}
	ret = g16_render_uat_verify(&g16_render_user_uat, G16_RENDER_POOL_VA,
		g16_render_pool, G16_RENDER_POOL_BYTES, J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS);
	if (ret)
		return ret;
	ret = g16_render_uat_verify(&g16_render_user_uat, G16_RENDER_GRAPH_VA & mask,
		g16_render_graph, G16_RENDER_GRAPH_BYTES, J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS);
	if (ret)
		return ret;
	ret = g16_render_uat_verify(&g16_render_fw_uat, G16_RENDER_GRAPH_VA & mask,
		g16_render_graph, G16_RENDER_GRAPH_BYTES, J713_UAT_FW_SHARED_FLAGS);
	if (ret)
		return ret;
	pr_info("g16_lab: render-prepare: software-verified unpublished UAT trees: user=%u tables/%u leaves firmware=%u tables/%u leaves\n",
		g16_render_user_uat.count, g16_render_user_uat.leaves,
		g16_render_fw_uat.count, g16_render_fw_uat.leaves);
	return 0;
}

/* Preparation only: no TTBR/PTE publication, MMIO, firmware or queue writes. */
static int g16_render_prepare(void)
{
	const struct firmware *fw;
	struct g16_render_job_args job = {};
	struct g16_render_pb_manager_args manager = {};
	u64 pages[G16_RENDER_POOL_BYTES / 0x8000];
	size_t next = 0x4000, resource_bytes = 0;
	unsigned int i, j;
	int ret;

	if (PAGE_SIZE != 0x4000 || !of_machine_is_compatible("apple,j713"))
		return -ENODEV;
	if (!render_frames || render_frames > ARRAY_SIZE(g16_render_frames))
		return -EINVAL;
	ret = request_firmware_direct(&fw, G16_RENDER_RESOURCE_FILE, NULL);
	if (ret)
		return ret;
	if (fw->size < 0x4000 || fw->size > 0x1000000 ||
	    memcmp(fw->data, "G16DRAW1", 8) ||
	    get_unaligned_le32(fw->data + 8) != G16_RENDER_RESOURCE_COUNT ||
	    get_unaligned_le32(fw->data + 12)) {
		ret = -EPROTO;
		goto out;
	}
	/* Validate the entire directory before allocating/copying any payloads. */
	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
		const u8 *entry = fw->data + 16 + i * 16;
		u64 va = get_unaligned_le64(entry);
		u32 size = get_unaligned_le32(entry + 8);
		u32 offset = get_unaligned_le32(entry + 12);

		if (!va || (va & 0x3fff) || !size || (size & 0x3fff) ||
		    va >= (1ULL << 42) || size > (1ULL << 42) - va ||
		    offset != next || next > fw->size || size > fw->size - next) {
			ret = -EPROTO;
			goto out;
		}
		for (j = 0; j < i; j++) {
			struct g16_render_resource *r = &g16_render_resources[j];

			if (va < r->va + r->size && r->va < va + size) {
				ret = -EPROTO;
				goto out;
			}
		}
		g16_render_resources[i].va = va;
		g16_render_resources[i].size = size;
		next += size;
	}
	if (next != fw->size) {
		ret = -EPROTO;
		goto out;
	}
	next = 0x4000;
	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
		struct g16_render_resource *r = &g16_render_resources[i];

		r->data = vzalloc(r->size);
		if (!r->data) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(r->data, fw->data + next, r->size);
		next += r->size;
		resource_bytes += r->size;
	}
	if (render_size != 4) {
		/* Allocate an isolated target rather than growing into the captured
		 * vertex/index mappings. No reference output enters this allocation.
		 */
		for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
			struct g16_render_resource *r = &g16_render_resources[i];
			u8 *data = r->data;
			u64 value;
			unsigned int j;

			if (r->va == 0x10000058000ULL) {
				u32 size = ALIGN(g16_render_target_offset() + render_size * render_size * 4,
						 PAGE_SIZE);
				void *target = vzalloc(size);

				if (!target) {
					ret = -ENOMEM;
					goto out;
				}
				resource_bytes = resource_bytes - r->size + size;
				vfree(r->data);
				r->data = target;
				r->size = size;
				r->va = g16_render_target_va();
			} else if (r->va == 0x10000138000ULL) {
				/* M4 fixture's texture descriptors and EOT PBE descriptor.
				 * Width/height positions match the independently decoded
				 * 4x4 words; preserve format, layout and swizzle bits.
				 */
				for (j = 0; j < 3; j++) {
					u32 offset = 0x20 + j * 0x300;
					u32 shift = j == 2 ? 24 : 28;
					u64 mask = ((1ULL << 28) - 1) << shift;

					value = g16_render_get(data + offset, 8);
					value = (value & ~mask) | (u64)(render_size - 1) << shift |
						(u64)(render_size - 1) << (shift + 14);
					g16_render_put(data + offset, value, 8);
					g16_render_put(data + offset + 8,
						(g16_render_target_va() + g16_render_target_offset()) >> 4, 8);
				}
			} else if (r->va == 0x10000168000ULL) {
				g16_render_put(data, render_size, 4);
				g16_render_put(data + 4, render_size, 4);
			} else if (r->va == 0x1000068000ULL) {
				/* Power-of-two size/2, emitted as IEEE754 without kernel FP. */
				u32 half = (ilog2(render_size) - 1 + 127) << 23;
				u32 max_tile = DIV_ROUND_UP(render_size, 32) - 1;

				/* PPP region clip precedes viewport control/transforms.
				 * Max X/Y are inclusive 32-pixel tile indices. Leaving
				 * the fixture's zero bounds clips everything to tile 0.
				 */
				g16_render_put(data + 0x904, BIT(31) | max_tile, 4);
				g16_render_put(data + 0x908, max_tile, 4);
				for (j = 0; j < 4; j++)
					g16_render_put(data + 0x910 + j * 4,
						half | (j == 3 ? BIT(31) : 0), 4);
			}
		}
		pr_info("g16_lab: render: generated %ux%u target=%#llx data-offset=%#x\n",
			render_size, render_size, g16_render_target_va(), g16_render_target_offset());
	}
	g16_render_graph = vzalloc(G16_RENDER_GRAPH_BYTES);
	g16_render_pool = vzalloc(G16_RENDER_POOL_BYTES);
	if (!g16_render_graph || !g16_render_pool) {
		ret = -ENOMEM;
		goto out;
	}
	/* prepareParameterBufferBlock uses getGPUVirtualAddressForUserProcess
	 * (vtable +160), the compact range-1 alias, before shifting by 15.
	 * The UAT mapping above uses the normal (+0x1000000000) address. */
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		pages[i] = G16_RENDER_POOL_VA - G16_RENDER_COMPACT_BASE + (u64)i * 0x8000;
	if (render_generated_shaders) {
		bool patched = false;

		for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
			struct g16_render_resource *r = &g16_render_resources[i];
			u8 *code = r->data;

			if (r->va != 0x10000000000ULL)
				continue;
			BUILD_BUG_ON(sizeof(g16_generated_vertex) > 0x80);
			BUILD_BUG_ON(sizeof(g16_generated_fragment) > 0x40);
			if (r->size < 0x5c0) {
				ret = -EINVAL;
				goto out;
			}
			memset(code + 0x540, 0, 0x80);
			memcpy(code + 0x540, g16_generated_vertex, sizeof(g16_generated_vertex));
			memset(code + 0x480, 0, 0x40);
			memcpy(code + 0x480, g16_generated_fragment, sizeof(g16_generated_fragment));
			patched = true;
		}
		if (!patched) {
			ret = -ENOENT;
			goto out;
		}
		pr_info("g16_lab: render: installed generated vertex/fragment bodies; reference bindings retained\n");
	}
	if (render_xy) {
		for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
			struct g16_render_resource *r = &g16_render_resources[i];
			u8 *data = r->data;

			if (r->va == 0x10000148000ULL) {
				BUILD_BUG_ON(sizeof(g16_generated_fragment_xy) > 0x1000);
				BUILD_BUG_ON(sizeof(g16_generated_fragment_triangle) > 0x1000);
				if (render_triangle)
					memcpy(data + 0x7000, g16_generated_fragment_triangle,
					       sizeof(g16_generated_fragment_triangle));
				else
					memcpy(data + 0x7000, g16_generated_fragment_xy,
					       sizeof(g16_generated_fragment_xy));
			} else if (r->va == 0x10000158000ULL) {
				/* M4 LoadShader main PC occupies bits 23..63 in units
				 * of 64 bytes. Preserve the low 23 control bits.
				 */
				u64 control = g16_render_get(data + 0x648, 8) & ((1ULL << 23) - 1);

				g16_render_put(data + 0x648, (0x1000014f000ULL << 17) | control, 8);
			} else if (r->va == 0x10000090000ULL && render_triangle) {
				/* (-1,-1), (1,-1), (-1,1): replace the oversized
				 * full-screen triangle with three clip-space vertices.
				 */
				g16_render_put(data + 8, 0x3f800000, 4);
				g16_render_put(data + 20, 0x3f800000, 4);
			}
		}
	}
	if (render_bindings) {
		for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
			struct g16_render_resource *r = &g16_render_resources[i];
			u8 *data = r->data;

			if (r->va == 0x10000148000ULL) {
				BUILD_BUG_ON(sizeof(g16_generated_vertex_loader) > 0x400);
				BUILD_BUG_ON(sizeof(g16_generated_fragment_loader) > 0x400);
				if (memchr_inv(data + 0x4000, 0, 0x3000)) {
					ret = -EINVAL;
					goto out;
				}
				memcpy(data + 0x4000, g16_generated_vertex_loader,
				       sizeof(g16_generated_vertex_loader));
				memcpy(data + 0x4400, g16_generated_fragment_loader,
				       sizeof(g16_generated_fragment_loader));
				g16_render_put(data + 0x5000, 0x1000014d100ULL, 8);
				/* Explicit float2 clip-space positions; draw arguments at
				 * +5200 stay zero. No captured vertex input is copied.
				 */
				for (j = 0; j < 6; j++)
					g16_render_put(data + 0x5100 + j * 4,
						       j == 2 || j == 5 ? 0x3f800000 : 0xbf800000, 4);
				memset(data, 0, sizeof(g16_generated_vertex_loader));
			} else if (r->va == 0x10000158000ULL) {
				memset(data + 0x640, 0, sizeof(g16_generated_fragment_loader));
			} else if (r->va == 0x1000018000ULL) {
				g16_render_put(data + 0xc, 0x14c000 >> 6, 4);
			} else if (r->va == 0x1000058000ULL) {
				g16_render_put(data + 0x14, 0x14c400 >> 6, 4);
			} else if (r->va == 0x10000128000ULL) {
				memset(data + 0xa0, 0, 8);
			} else if (r->va == 0x10000090000ULL) {
				memset(data, 0, 24);
			}
		}
		pr_info("g16_lab: render: generated relocated loaders and vertex bindings; old entries erased\n");
	}
	if (render_stream) {
		for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
			struct g16_render_resource *r = &g16_render_resources[i];
			u8 *data = r->data;

			if (r->va == 0x1000018000ULL) {
				if (memchr_inv(data + 0x4000, 0, 0x1000)) {
					ret = -EINVAL;
					goto out;
				}
				ret = g16_render_pack_stream(data + 0x4000, 0x1000,
							     0x1000014c000ULL, 3, 0);
				if (ret < 0)
					goto out;
				memset(data, 0, G16_RENDER_STREAM_BYTES);
			} else if (r->va == 0x10000148000ULL) {
				/* A second, fixed oversized triangle follows the first.
				 * Frames select 0..2 or 3..5 through the draw command.
				 */
				for (j = 0; j < 6; j++)
					g16_render_put(data + 0x5118 + j * 4,
						       j == 2 || j == 5 ? 0x40400000 : 0xbf800000, 4);
			}
		}
		pr_info("g16_lab: render: generated direct VDM stream at 0x1c000; old stream erased\n");
	}
	if (render_varying) {
		for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
			struct g16_render_resource *r = &g16_render_resources[i];
			u8 *data = r->data;

			if (r->va == 0x10000000000ULL) {
				memset(data + 0x540, 0, 0x80);
			} else if (r->va == 0x10000148000ULL) {
				BUILD_BUG_ON(sizeof(g16_generated_vertex_varying) > 0x800);
				memcpy(data + 0x4000, g16_generated_vertex_varying_loader,
				       sizeof(g16_generated_vertex_varying_loader));
				memcpy(data + 0x5800, g16_generated_vertex_varying,
				       sizeof(g16_generated_vertex_varying));
				BUILD_BUG_ON(sizeof(g16_generated_fragment_varying) > 0x1000);
				memset(data + 0x7000, 0, 0x1000);
				memcpy(data + 0x7000, g16_generated_fragment_varying,
				       sizeof(g16_generated_fragment_varying));
				memset(data + 0x5100, 0, 0x60);
				for (j = 0; j < 3; j++) {
					g16_render_put(data + 0x5100 + j * 16,
						       j == 1 ? 0x3f800000 : 0xbf800000, 4);
					g16_render_put(data + 0x5104 + j * 16,
						       j == 2 ? 0x3f800000 : 0xbf800000, 4);
					g16_render_put(data + 0x5108 + j * 16,
						       (render_varying_uniform || j == 1) ? 0x3f800000 : 0, 4);
				}
				/* Mesa CF_BINDING: RHW slot + one user slot, one
				 * coefficient; scalar linear binding from user slot 1.
				 */
				g16_render_put(data + 0x5400, 0x102, 4);
				g16_render_put(data + 0x5404, 0x10c, 4);
			} else if (r->va == 0x1000018000ULL) {
				g16_render_put(data + 0x4010, 0x505, 4);
			} else if (r->va == 0x1000058000ULL) {
				/* HAL200 fragment record: one binding, absolute VIT.
				 * Output select enables interpolation; UVS has 5 words.
				 */
				g16_render_put(data + 4, 0x10100, 4);
				g16_render_put(data + 8, 0x14d400, 4);
				g16_render_put(data + 0x20, 0x10000, 4);
				g16_render_put(data + 0x2c, 5, 4);
			}
		}
		pr_info("g16_lab: render: linear vertex varying, fixed geometry, coefficient table at 0x1000014d400\n");
	}
	if (render_ta_probe && !render_generated_shaders) {
		bool patched = false;

		/* Prefix the 42-byte store sequence, preserving all 86 captured bytes.
		 * The straight-line VS overwrites r1 before use; r2/r3/r8 are dead;
		 * its uniform argument pointer and special-register inputs are untouched. */
		BUILD_BUG_ON(sizeof(g16_vertex_marker_code) > 0x5c0 - 0x594);
		for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
			struct g16_render_resource *r = &g16_render_resources[i];
			u8 *code = r->data;

			if (r->va != 0x10000000000ULL)
				continue;
			if (r->size < 0x5c0 || code[0x594] != 0x0e || code[0x595] ||
			    memchr_inv(code + 0x596, 0, 0x5c0 - 0x596)) {
				ret = -EINVAL;
				goto out;
			}
			memmove(code + 0x540 + sizeof(g16_vertex_marker_code) - 2,
				code + 0x540, 0x56);
			memcpy(code + 0x540, g16_vertex_marker_code, sizeof(g16_vertex_marker_code) - 2);
			patched = true;
		}
		if (!patched) {
			ret = -ENOENT;
			goto out;
		}
	}
	if (render_loader_marker) {
		bool loader = false, stream = false;

		/* Relocate the straight-line first ESL into unused space, retaining
		 * all other captured helpers and its absolute shader/binding pointers. */
		for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
			struct g16_render_resource *r = &g16_render_resources[i];
			u8 *code = r->data;

			if (r->va == 0x10000148000ULL) {
				if (r->size < 0x8000 || code[0xa2] != 0x0e ||
				    memchr_inv(code + 0x7000, 0, 0x1000)) {
					ret = -EINVAL;
					goto out;
				}
				memcpy(code + 0x7000, g16_vertex_loader_marker_code,
				       sizeof(g16_vertex_loader_marker_code));
				memcpy(code + 0x7000 + sizeof(g16_vertex_loader_marker_code),
				       code, 0xa4);
				loader = true;
			} else if (r->va == 0x1000018000ULL) {
				if (g16_render_get(code + 0xc, 4) != 0x5200) {
					ret = -EINVAL;
					goto out;
				}
				g16_render_put(code + 0xc, 0x53c0, 4);
				stream = true;
			}
		}
		if (!loader || !stream) {
			ret = -ENOENT;
			goto out;
		}
	}
	ret = g16_render_pack_page_list(g16_render_graph + 0xa000, 0x1000,
					pages, ARRAY_SIZE(pages));
	if (ret)
		goto out;

	manager.page_list_fw_va = G16_RENDER_GRAPH_VA + 0xa000;
	/* Native halLoadPBDesc preserves bit 42 to select the upper root. */
	manager.page_list_gpu_va = manager.page_list_fw_va;
	manager.page_list_bytes = 0x1000;
	manager.page_count = ARRAY_SIZE(pages);
	manager.block_capacity = 64;
	manager.block_wptr = ARRAY_SIZE(pages) / 4;
	manager.block_list_va = G16_RENDER_GRAPH_VA + 0xb000;
	manager.block_ring_va = G16_RENDER_GRAPH_VA + 0xb800;
	manager.counter_va = G16_RENDER_GRAPH_VA + 0xb900;
	manager.min_pages = manager.page_count;
	manager.max_pages = manager.page_count;
	ret = g16_render_pack_pb_manager(g16_render_graph + 0x9000, 0xc0, &manager);
	if (ret)
		goto out;
	for (i = 0; i < ARRAY_SIZE(pages) / 4; i++)
		g16_render_put(g16_render_graph + 0xb000 + i * 8, pages[i * 4] >> 15, 4);
	g16_render_put(g16_render_graph + 0xb800, manager.block_wptr, 4);
	g16_render_put(g16_render_graph + 0xb804, manager.block_wptr, 4);
	/* buildPageList leaves the host hardware-ring state completely zero. */
	ret = g16_render_pack_pb_hw(g16_render_graph + G16_RENDER_PB_HW, 16,
		manager.page_list_gpu_va & ((1ULL << 43) - 1),
		manager.page_count, 0, 0, 0, 0);
	if (ret)
		goto out;
	ret = g16_render_pack_init_pb(g16_render_graph + G16_RENDER_INIT_PB,
		0x20, 6, 0, manager.block_rptr, manager.block_wptr,
		G16_RENDER_GRAPH_VA + 0x9000, 0x100);
	if (ret)
		goto out;

	/* Addresses below are planned mappings. They are NOT installed in UAT. */
	job.notifier_va = G16_RENDER_GRAPH_VA + 0xc000;
	job.shared_stamp_va = G16_RENDER_GRAPH_VA + 0xc800;
	job.pb_aux_va = G16_RENDER_GRAPH_VA + 0x9300;
	job.context_generation = 1;
	job.sku.pb_slot_va = G16_RENDER_GRAPH_VA + 0x9200;
	/* loadParameterManagementData: this is the actual 0xc0 manager. */
	job.sku.buffer_info_va = G16_RENDER_GRAPH_VA + 0x9000;
	job.sku.context_id = 6;
	job.sku.stamp = 0x100;
	job.sku.fragment_increment = 1;
	for (i = 0; i < 2; i++) {
		unsigned int command_offset = i ? 0x4000 : 0;
		unsigned int sku_offset = i ? 0x8400 : 0x8000;
		struct g16_render_register first = {i ? 0x1738 : 0x1748, i ? 1 : 0, 1};

		job.sku.command_va = G16_RENDER_GRAPH_VA + command_offset;
		job.sku.sku_va = G16_RENDER_GRAPH_VA + sku_offset;
		job.sku.queue_va = G16_RENDER_GRAPH_VA + G16_RENDER_QUEUE(i);
		job.sku.slot_array_va = G16_RENDER_GRAPH_VA + G16_RENDER_SLOT_ARRAY(i) + (i ? 8 : 4);
		job.sku.stamp_va = G16_RENDER_GRAPH_VA + 0xc900 + i * 8;
		job.sku.uuid = 0x16000001 + i;
		job.sku.stamp_slot = i;
		ret = g16_render_pack_sku(g16_render_graph + sku_offset, 0x400, i, &job.sku);
		if (ret < 0)
			goto out;
		ret = g16_render_pack_job(g16_render_graph + command_offset, 0x4000, i, &job, ret);
		if (ret)
			goto out;
		ret = g16_render_pack_registers(g16_render_graph + command_offset,
			0x4000, i, job.sku.command_va & ((1ULL << 42) - 1), &first, 1);
		if (ret)
			goto out;
	}
	ret = g16_render_prepare_uat();
	if (ret)
		goto out;
	g16_render_graph_blob.data = g16_render_graph;
	g16_render_graph_blob.size = G16_RENDER_GRAPH_BYTES;
	g16_render_graph_file = debugfs_create_blob("g16_lab_render_prepared", 0400,
						   NULL, &g16_render_graph_blob);
	if (IS_ERR_OR_NULL(g16_render_graph_file)) {
		ret = g16_render_graph_file ? PTR_ERR(g16_render_graph_file) : -ENOMEM;
		g16_render_graph_file = NULL;
		goto out;
	}
	g16_render_output_dir = debugfs_create_dir("g16_lab_render", NULL);
	if (IS_ERR(g16_render_output_dir)) {
		ret = PTR_ERR(g16_render_output_dir);
		g16_render_output_dir = NULL;
		goto out;
	}
	g16_render_pool_blob.data = g16_render_pool;
	g16_render_pool_blob.size = G16_RENDER_POOL_BYTES;
	debugfs_create_blob("pool.bin", 0400, g16_render_output_dir, &g16_render_pool_blob);
	if (render_triangle) {
		for (i = 0; i < render_frames; i++) {
			char name[32];

			g16_render_frames[i].size = 256 * 256 * 4;
			g16_render_frames[i].data = vzalloc(g16_render_frames[i].size);
			if (!g16_render_frames[i].data) {
				ret = -ENOMEM;
				goto out;
			}
			snprintf(name, sizeof(name), "frame-%02u.bin", i);
			debugfs_create_blob(name, 0400, g16_render_output_dir, &g16_render_frames[i]);
		}
	}
	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
		struct g16_render_resource *r = &g16_render_resources[i];
		char name[32];

		r->blob.data = r->data;
		r->blob.size = r->size;
		snprintf(name, sizeof(name), "resource-%llx.bin", r->va);
		debugfs_create_blob(name, 0400, g16_render_output_dir, &r->blob);
	}
	pr_info("g16_lab: render-prepare: %u capture buffers (%zu bytes), %u pool pages, TA/fragment packet envelopes prepared; UAT unpublished, no GPU submission\n",
		G16_RENDER_RESOURCE_COUNT, resource_bytes, (unsigned int)ARRAY_SIZE(pages));
	ret = 0;
out:
	release_firmware(fw);
	if (ret)
		g16_render_release();
	return ret;
}
#endif
