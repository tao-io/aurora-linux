/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_SUBMIT_H
#define G16_RENDER_SUBMIT_H

/* First live gate: independent TA and fragment queues, no render commands yet. */
static bool g16_render_attached;
static bool g16_render_context_published;
static struct page *g16_render_upper_root;
static u8 g16_render_saved_pipes[0x40];
static u8 g16_render_saved_pb_table[16];
static int g16_render_test_ta(void __iomem *asc, u64 crashlog_iova,
			    unsigned int crashlog_pages);

static u64 g16_render_pb_table_va(bool gpu)
{
	u64 va = G16_RENDER_GRAPH_VA + G16_RENDER_PB_HW;

	return gpu ? va & ((1ULL << 42) - 1) : va;
}

static void g16_render_clean_tree(struct g16_render_uat *u)
{
	unsigned int i;

	for (i = 0; i < u->count; i++)
		g16_lab_dcache_clean(page_address(u->tables[i]), PAGE_SIZE);
}

static void g16_render_initialize_queue(unsigned int engine)
{
	u8 *q = g16_render_graph + G16_RENDER_QUEUE(engine);
	u8 *state = g16_render_graph + G16_RENDER_QUEUE_STATE(engine);
	u8 *list = g16_render_graph + G16_RENDER_NOTIFIER_LIST(engine);
	u64 base = G16_RENDER_GRAPH_VA;

	BUILD_BUG_ON(J713_INITDATA_QUEUE_INFO_SIZE > 0x4000);
	BUILD_BUG_ON(J713_INITDATA_PIPE_RING_SIZE > 0x2000);
	g16_render_put(q, base + G16_RENDER_QUEUE_STATE(engine), 8);
	g16_render_put(q + 8, base + G16_RENDER_QUEUE_RING(engine), 8);
	g16_render_put(q + 0x10, base + G16_RENDER_NOTIFIER_LIST(engine), 8);
	g16_render_put(q + 0x18, base + G16_RENDER_QUEUE(engine) + 0xb0, 8);
	g16_render_put(q + J713_QUEUE_EVENT_SLOT_OFFSET, U32_MAX, 4);
	memcpy(q + J713_QUEUE_PRIORITY_OFFSET, &g16_normal_compute_priority,
	       sizeof(g16_normal_compute_priority));
	g16_render_put(q + 0x4c, U32_MAX, 4);
	g16_render_put(q + J713_QUEUE_CONTEXT_PTR_OFFSET,
		       base + G16_RENDER_QUEUE_CONTEXT(engine), 8);
	g16_render_put(state + J713_QUEUE_RING_SIZE_OFFSET,
		       J713_INITDATA_QUEUE_RING_COUNT, 4);
	g16_render_put(list + 8, base + G16_RENDER_NOTIFIER_LIST(engine), 8);
	g16_lab_initialize_scheduler_context(g16_render_graph +
					     G16_RENDER_QUEUE_CONTEXT(engine));
}

/* Called only while the existing initdata owner still holds all its backing. */
static int g16_render_attach(u64 scratch_va)
{
	u64 *fw, *upper, *user;
	u8 *runtime;
	unsigned int i;

	if (!g16_render_graph || !g16_render_fw_uat.count ||
	    !g16_render_user_uat.count || !g16_initdata_live_shared_root ||
	    !g16_initdata_live_kernel_lower_root ||
	    !g16_initdata_live_user_root ||
	    !g16_initdata_live_user_ttbr1_root_phys || g16_render_attached)
		return -EINVAL;
	BUILD_BUG_ON(((G16_RENDER_GRAPH_VA >> 36) & 63) != 3);
	fw = page_address(g16_render_fw_uat.tables[0]);
	user = page_address(g16_render_user_uat.tables[0]);
	runtime = g16_lab_live_object_ptr(g16_initdata_live_runtime_first, 0);
	if (g16_initdata_live_shared_root[3] ||
	    g16_initdata_live_kernel_lower_root[3] || !fw[3] || user[7]) {
		pr_err("g16_lab: render: attachment collision shared=%#llx lower=%#llx own=%#llx user7=%#llx\n",
			g16_initdata_live_shared_root[3],
			g16_initdata_live_kernel_lower_root[3], fw[3], user[7]);
		return -EBUSY;
	}
	/* Unused channels point to initdata's zero scratch page, not NULL. */
	for (i = 0; i < 0x40; i += 8) {
		if (g16_render_get(runtime + J713_RUNTIME_TA_PIPE_DESC_OFFSET + i, 8) != scratch_va) {
			pr_err("g16_lab: render: runtime pipe +%#x is not the unused scratch descriptor\n", i);
			return -EBUSY;
		}
	}
	g16_render_upper_root = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!g16_render_upper_root)
		return -ENOMEM;
	upper = page_address(g16_render_upper_root);
	/* Borrow resident firmware/UMA mappings; only this root page is owned. */
	memcpy(upper, phys_to_virt(g16_initdata_live_user_ttbr1_root_phys), PAGE_SIZE);
	if (upper[3]) {
		pr_err("g16_lab: render: upper root-3 collision=%#llx\n", upper[3]);
		__free_page(g16_render_upper_root);
		g16_render_upper_root = NULL;
		return -EBUSY;
	}
	/* The GPU upper alias needs writable data leaves, not firmware AP1. */
	upper[3] = user[3];
	user[7] = g16_initdata_live_user_root[7];
	for (i = 0; i < 2; i++)
		g16_render_initialize_queue(i);
	g16_render_clean_tree(&g16_render_user_uat);
	g16_render_clean_tree(&g16_render_fw_uat);
	g16_lab_dcache_clean(upper, PAGE_SIZE);
	g16_lab_dcache_clean(g16_render_graph, G16_RENDER_GRAPH_BYTES);
	g16_lab_dcache_clean(g16_render_pool, G16_RENDER_POOL_BYTES);
	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++)
		g16_lab_dcache_clean(g16_render_resources[i].data,
				     g16_render_resources[i].size);
	memcpy(g16_render_saved_pipes, runtime + J713_RUNTIME_TA_PIPE_DESC_OFFSET,
	       sizeof(g16_render_saved_pipes));
	/* Root 3 is disjoint from resident firmware (0) and lab initdata (2). */
	WRITE_ONCE(g16_initdata_live_shared_root[3], fw[3]);
	WRITE_ONCE(g16_initdata_live_kernel_lower_root[3], user[3]);
	g16_lab_dcache_clean(g16_initdata_live_shared_root, PAGE_SIZE);
	g16_lab_dcache_clean(g16_initdata_live_kernel_lower_root, PAGE_SIZE);
	for (i = 0; i < 2; i++) {
		u8 *pipe = runtime + J713_RUNTIME_TA_PIPE_DESC_OFFSET + i * 0x20;
		u64 state = G16_RENDER_GRAPH_VA + G16_RENDER_PIPE_STATE(i);

		g16_render_put(pipe, state + J713_PIPE_CONSUMER_OFFSET, 8);
		g16_render_put(pipe + 8, state + J713_PIPE_CFI_OFFSET, 8);
		g16_render_put(pipe + 0x10, state + J713_PIPE_PRODUCER_OFFSET, 8);
		g16_render_put(pipe + 0x18,
			       G16_RENDER_GRAPH_VA + G16_RENDER_PIPE_RING(i), 8);
	}
	memcpy(g16_render_saved_pb_table, runtime + J713_RUNTIME_PB_TABLE_GPU_OFFSET, 16);
	if (render_ta_probe) {
		g16_render_put(runtime + J713_RUNTIME_PB_TABLE_GPU_OFFSET,
			g16_render_pb_table_va(true), 8);
		g16_render_put(runtime + J713_RUNTIME_PB_TABLE_FIRMWARE_OFFSET,
			g16_render_pb_table_va(false), 8);
	}
	g16_lab_dcache_clean(runtime, J713_INITDATA_RUNTIME_SIZE);
	dma_wmb();
	g16_render_attached = true;
	pr_info("g16_lab: render: attached independent TA/fragment pipes and root-3 graph before firmware boot\n");
	return 0;
}

/* The enclosing bootstrap has stopped ASC before this function is called. */
static void g16_render_detach(void)
{
	u8 *runtime;
	unsigned int i;

	if (!g16_render_attached)
		return;
	if (g16_render_context_published) {
		struct g16_uat_context_slot *slot =
			g16_initdata_live_uat_kernel_context + 6;

		WRITE_ONCE(slot->ttbr0, 0);
		WRITE_ONCE(slot->ttbr1, 0);
		g16_lab_dcache_clean(slot, sizeof(*slot));
		g16_render_context_published = false;
	}
	runtime = g16_lab_live_object_ptr(g16_initdata_live_runtime_first, 0);
	memcpy(runtime + J713_RUNTIME_PB_TABLE_GPU_OFFSET, g16_render_saved_pb_table, 16);
	memcpy(runtime + J713_RUNTIME_TA_PIPE_DESC_OFFSET, g16_render_saved_pipes,
	       sizeof(g16_render_saved_pipes));
	g16_lab_dcache_clean(runtime, J713_INITDATA_RUNTIME_SIZE);
	WRITE_ONCE(g16_initdata_live_shared_root[3], 0);
	WRITE_ONCE(g16_initdata_live_kernel_lower_root[3], 0);
	g16_lab_dcache_clean(g16_initdata_live_shared_root, PAGE_SIZE);
	g16_lab_dcache_clean(g16_initdata_live_kernel_lower_root, PAGE_SIZE);
	dma_wmb();
	g16_lab_uat_tlbi_context(6);
	g16_lab_uat_tlbi_context(0);
	g16_lab_dcache_invalidate(g16_render_graph, G16_RENDER_GRAPH_BYTES);
	g16_lab_dcache_invalidate(g16_render_pool, G16_RENDER_POOL_BYTES);
	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++)
		g16_lab_dcache_invalidate(g16_render_resources[i].data,
					 g16_render_resources[i].size);
	__free_page(g16_render_upper_root);
	g16_render_upper_root = NULL;
	g16_render_attached = false;
	pr_info("g16_lab: render: removed context-6 and root-3 mappings after firmware stop\n");
}

static int g16_render_publish_context(void)
{
	struct g16_uat_context_slot *slot = g16_initdata_live_uat_kernel_context + 6;
	u64 ttbr0, ttbr1;
	int ret;

	if (!g16_render_attached || !g16_initdata_live_uat_handoff)
		return -EINVAL;
	ret = g16_lab_uat_handoff_lock(g16_initdata_live_uat_handoff);
	if (ret)
		return ret;
	g16_lab_dcache_invalidate(slot, sizeof(*slot));
	if (READ_ONCE(slot->ttbr0) || READ_ONCE(slot->ttbr1)) {
		g16_lab_uat_handoff_unlock(g16_initdata_live_uat_handoff);
		return -EBUSY;
	}
	ttbr0 = page_to_phys(g16_render_user_uat.tables[0]) |
		J713_UAT_TTBR_ASID(6) | J713_UAT_TTBR_VALID;
	ttbr1 = page_to_phys(g16_render_upper_root) |
		J713_UAT_TTBR_ASID(6) | J713_UAT_TTBR_VALID;
	WRITE_ONCE(slot->ttbr0, ttbr0);
	WRITE_ONCE(slot->ttbr1, ttbr1);
	g16_lab_dcache_clean(slot, sizeof(*slot));
	dma_wmb();
	g16_render_context_published = true;
	g16_lab_uat_handoff_unlock(g16_initdata_live_uat_handoff);
	g16_lab_uat_tlbi_context(6);
	g16_lab_dcache_invalidate(slot, sizeof(*slot));
	if (READ_ONCE(slot->ttbr0) != ttbr0 || READ_ONCE(slot->ttbr1) != ttbr1)
		return -EIO;
	pr_info("g16_lab: render: published context-6 under UAT handoff ttbr=%#llx/%#llx\n",
		ttbr0, ttbr1);
	return 0;
}

static int g16_render_test_queue(void __iomem *asc, unsigned int engine,
				 u64 crashlog_iova, unsigned int crashlog_pages)
{
	u8 *q = g16_render_graph + G16_RENDER_QUEUE(engine);
	u8 *state = g16_render_graph + G16_RENDER_QUEUE_STATE(engine);
	u8 *pipe = g16_render_graph + G16_RENDER_PIPE_RING(engine);
	u8 *pipe_state = g16_render_graph + G16_RENDER_PIPE_STATE(engine);
	u8 *barrier = g16_render_graph + G16_RENDER_BARRIER(engine);
	unsigned long deadline = jiffies + msecs_to_jiffies(1000), settled = 0;
	u64 timestamp, message;
	u32 endpoint;
	int ret;

	g16_render_put(barrier, 4, 4);
	g16_render_put(barrier + 4, G16_RENDER_GRAPH_VA + 0xca00, 8);
	g16_render_put(barrier + 0xc, G16_RENDER_GRAPH_VA + 0xca00, 8);
	g16_render_put(barrier + 0x20, 0x80, 4);
	/* An empty-queue barrier seeds DAG cursors from +24. The following
	 * TA job uses stamp 0x100, so seed index one before registering it. */
	if (render_ta_probe && !engine)
		g16_render_put(barrier + 0x24, 0x100, 4);
	g16_render_put(g16_render_graph + G16_RENDER_QUEUE_RING(engine),
		G16_RENDER_GRAPH_VA + G16_RENDER_BARRIER(engine), 8);
	g16_lab_dcache_clean(g16_render_graph, G16_RENDER_GRAPH_BYTES);
	dma_wmb();
	g16_render_put(state + J713_QUEUE_RING_CPU_WPTR_OFFSET, 1, 4);
	g16_lab_dcache_clean(state, J713_INITDATA_QUEUE_RING_STATE_SIZE);
	dma_wmb();
	asm volatile("mrs %0, cntpct_el0" : "=r"(timestamp));
	g16_render_put(pipe, timestamp, 8);
	g16_render_put(pipe + 8, G16_RENDER_GRAPH_VA + G16_RENDER_QUEUE(engine), 8);
	g16_render_put(pipe + 0x10, engine, 4);
	g16_render_put(pipe + 0x14, 1, 2);
	pipe[J713_PIPE_ENTRY_SCHEDULER_SLOT_OFFSET] = 0x70 + engine;
	pipe[J713_PIPE_ENTRY_NEW_QUEUE_OFFSET] = 1;
	g16_lab_dcache_clean(pipe, J713_INITDATA_PIPE_RING_SIZE);
	dma_wmb();
	g16_render_put(pipe_state + J713_PIPE_PRODUCER_OFFSET, 1, 4);
	g16_lab_dcache_clean(pipe_state, J713_INITDATA_PIPE_STATE_SIZE);
	dma_wmb();
	ret = g16_asc_send(asc, J713_PIPE_KICK_TA_GROUP0 | engine, 0x21);
	if (ret)
		return ret;
	pr_info("g16_lab: render: submitted %s queue barrier selector=%u\n",
		engine ? "fragment" : "TA", 0x70 + engine);
	do {
		ret = g16_lab_drain_statistics_ring();
		if (ret || g16_firmware_recovery_observed)
			return ret ? ret : -EIO;
		g16_lab_dcache_invalidate(pipe_state, J713_INITDATA_PIPE_STATE_SIZE);
		g16_lab_dcache_invalidate(q, J713_INITDATA_QUEUE_INFO_SIZE);
		g16_lab_dcache_invalidate(state, J713_INITDATA_QUEUE_RING_STATE_SIZE);
		if (g16_render_get(pipe_state, 4) == 1 &&
		    g16_render_get(pipe_state + J713_PIPE_CFI_OFFSET, 4) == 1 &&
		    g16_render_get(q + J713_QUEUE_READER_OFFSET, 4) == 1 &&
		    g16_render_get(q + J713_QUEUE_CFI_OFFSET, 4) == 1 &&
		    g16_render_get(q + J713_QUEUE_PRODUCER_OFFSET, 4) == 1 &&
		    g16_render_get(state + J713_QUEUE_RING_GPU_RPTR_OFFSET, 4) == 1 &&
		    !g16_render_get(q + J713_QUEUE_BUSY_OFFSET, 4)) {
			if (!settled)
				settled = jiffies + msecs_to_jiffies(50);
			else if (time_after_eq(jiffies, settled)) {
				pr_info("g16_lab: render: %s barrier retired, queue idle for 50ms; no shader submitted\n",
					engine ? "fragment" : "TA");
				return 0;
			}
		} else {
			settled = 0;
		}
		ret = g16_asc_recv_timeout(asc, &message, &endpoint, 10000);
		if (!ret && endpoint == 1 &&
		    FIELD_GET(RTKIT_MGMT_TYPE, message) == RTKIT_BUFFER_REQUEST &&
		    crashlog_iova && FIELD_GET(RTKIT_BUFFER_IOVA, message) == crashlog_iova) {
			g16_lab_capture_crashlog(crashlog_iova, crashlog_pages);
			return -EIO;
		}
		if (ret && ret != -ETIMEDOUT)
			return ret;
	} while (time_before(jiffies, deadline));
	pr_err("g16_lab: render: queue %u timed out pipe=%llu/%llu q=%llu/%llu/%llu busy=%llu ring=%llu\n",
		engine, g16_render_get(pipe_state, 4),
		g16_render_get(pipe_state + J713_PIPE_CFI_OFFSET, 4),
		g16_render_get(q + J713_QUEUE_READER_OFFSET, 4),
		g16_render_get(q + J713_QUEUE_CFI_OFFSET, 4),
		g16_render_get(q + J713_QUEUE_PRODUCER_OFFSET, 4),
		g16_render_get(q + J713_QUEUE_BUSY_OFFSET, 4),
		g16_render_get(state + J713_QUEUE_RING_GPU_RPTR_OFFSET, 4));
	return -ETIMEDOUT;
}

static int g16_render_queue_gate(void __iomem *asc, u64 crashlog_iova,
				 unsigned int crashlog_pages)
{
	int ret = g16_lab_compute_wfi(asc, crashlog_iova, crashlog_pages);

	if (ret || READ_ONCE(stage_result))
		return ret;
	ret = g16_render_publish_context();
	if (!ret)
		ret = g16_render_test_queue(asc, 0, crashlog_iova, crashlog_pages);
	if (!ret)
		ret = g16_render_test_queue(asc, 1, crashlog_iova, crashlog_pages);
	if (!ret && render_ta_probe)
		ret = g16_render_test_ta(asc, crashlog_iova, crashlog_pages);
	WRITE_ONCE(stage_result, ret);
	pr_info("g16_lab: render: independent queue gate result=%d\n", ret);
	/* Keep diagnostics readable; the outer bootstrap always stops firmware. */
	return 0;
}
#include "g16_render_draw.h"
#endif
