/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_PREPARE_H
#define G16_RENDER_PREPARE_H

#include "g16_render_pack.h"
#include "g16_render_ta.h"
#include "g16_render_fragment.h"
#include "g16_render_geometry.h"
#include "g16_render_uat.h"

#define G16_RENDER_RESOURCE_COUNT 34
#define G16_RENDER_GRAPH_BYTES 0x80000
#define G16_RENDER_POOL_BYTES 0x400000
#define G16_RENDER_GRAPH_VA 0xfffffc3000000000ULL
#define G16_RENDER_POOL_VA 0x6000000000ULL
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
};

static struct g16_render_resource g16_render_resources[G16_RENDER_RESOURCE_COUNT];
static u8 *g16_render_graph;
static void *g16_render_pool;
static struct dentry *g16_render_graph_file;
static struct debugfs_blob_wrapper g16_render_graph_blob;
static struct g16_render_uat g16_render_user_uat;
static struct g16_render_uat g16_render_fw_uat;

static void g16_render_release(void)
{
	unsigned int i;

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
	return r->va == 0x1000000000ULL ? J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS :
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
	g16_render_graph = vzalloc(G16_RENDER_GRAPH_BYTES);
	g16_render_pool = vzalloc(G16_RENDER_POOL_BYTES);
	if (!g16_render_graph || !g16_render_pool) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		pages[i] = G16_RENDER_POOL_VA + (u64)i * 0x8000;
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
