/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_DRAW_H
#define G16_RENDER_DRAW_H

/* Experimental reconstructed single-cluster, single-layer tiling job. */
static u64 g16_render_gpu_address(u64 va)
{
	return va & ((1ULL << 43) - 1);
}

static int g16_render_build_ta(void)
{
	struct g16_render_geometry geometry;
	struct g16_render_ta_platform platform = {
		.accelerator_count_4e4 = 1,
		.default_d418 = 1, /* G16G vtable +1098: halGetUmaDefaultDupmMinCount */
	};
	struct g16_render_job_args job = {};
	struct g16_fw_compute_notifier *notifier = (void *)(g16_render_graph + 0xc000);
	struct g16_render_register *regs;
	u8 *d, *work = g16_render_graph;
	u64 base = G16_RENDER_GRAPH_VA, mode;
	int count, ret;

	d = kzalloc(G16_RENDER_TA_DESCRIPTOR_BYTES, GFP_KERNEL);
	regs = kcalloc(128, sizeof(*regs), GFP_KERNEL);
	if (!d || !regs) {
		ret = -ENOMEM;
		goto out;
	}
	ret = g16_render_geometry_single(&geometry, g16_render_width(), g16_render_height(), 32, 32, 1);
	if (ret)
		goto out;
#define D(o, v, n) g16_render_put(d + (o), (v), (n))
#define GPU(o) g16_render_gpu_address(base + (o))
#define TILER(o) (((base + (o)) & ((1ULL << 42) - 1)) - G16_RENDER_COMPACT_BASE)
	/* Native normal -> compact range-1 address for captured VDM resource 29. */
	D(0xea8, g16_render_vdm_base(), 8);
	/* TA consumes compact range-1 pointers here, like its tilemap. A normal
	 * pointer made SuperTux's first preemption write land at 0x4000050000,
	 * one compact-range base above the mapped graph at 0x3000050000.
	 * Zero-backed dedicated storage; the 64 KiB sizes remain provisional. */
	D(0xf48, TILER(0x40000), 8);
	D(0xf50, TILER(0x50000), 8);
	/* Same-M4 Metal endRenderPassCommon raw segment +b8/+c0. */
	D(0xf88, 0x003a0012006b0003ULL, 8);
	D(0xf90, 1, 4);
	/* loadRenderTargetMemoryData takes the compact (+160) address for TA;
	 * fragment uses the corresponding normal (+158) address instead. */
	D(0x1000, TILER(g16_render_tpc_offset()), 8);
	D(0x1008, base + g16_render_tpc_offset(), 8);
	D(0x1010, geometry.tpc_stride, 4);
	D(0x1018, TILER(g16_render_tilemap_offset()), 8);
	D(0x1020, geometry.region_stride, 4);
	D(0x1028, geometry.tpc_bytes, 8);
	D(0x1030, TILER(0x29000), 8);
	D(0x1038, 1, 4);
	D(0x1040, TILER(0x29000), 8);
	D(0x10a8, geometry.utile_config, 8);
	if (g16_drm_active_job && (g16_drm_active_job->render.flags & BIT(4))) {
		const struct drm_asahi_cmd_render *r = &g16_drm_active_job->render;

		D(0x10b0, r->ppp_multisamplectl, 8);
		/* M4 generateRegisterListForTA derives PPP_CTRL[9] from this
		 * accelerator flag and bit 0 from descriptor+ff8. */
		platform.accelerator_flags_6a8 |= (u64)(r->ppp_ctrl & 0x200) << 7;
		D(0xff8, r->ppp_ctrl & 1, 1);
	}
	D(0x10b8, geometry.pixels, 4);
	D(0x10bc, geometry.screen, 4);
	D(0x10c0, geometry.x_blocks, 4);
	D(0x10c4, geometry.y_blocks, 4);
	D(0x10c8, geometry.blocks, 4);
	D(0x7a4, U32_MAX, 4);
	/* Native per-pass PB state starts with GPU/FW aliases of its scratch. */
	g16_render_put(work + 0x9200, GPU(0x24000), 8);
	g16_render_put(work + 0x9208, base + 0x24000, 8);
	/* Firmware 25G83 completion at 22e94 unconditionally dereferences
	 * PB slot +40 for the per-pass statistics record (at least 0x24 bytes).
	 * Keep it separate from the command's auxiliary record at +9300.
	 */
	g16_render_put(work + 0x9240, base + 0x9400, 8);
	platform.pb_slot_word_00 = GPU(0x24000);
	platform.pb_slot_word_28 = 0;
	job.notifier_va = base + 0xc000;
	job.shared_stamp_va = base + 0xc800;
	job.pb_aux_va = base + 0x9300;
	job.context_generation = 1;
	job.command_counter = g16_render_frame * 2 + 1;
	job.sku.command_va = base;
	job.sku.sku_va = base + 0x8000;
	job.sku.queue_va = base + G16_RENDER_QUEUE(0);
	job.sku.slot_array_va = base + G16_RENDER_SLOT_ARRAY(0) + 4;
	job.sku.pb_slot_va = base + 0x9200;
	job.sku.buffer_info_va = base + 0x9000;
	job.sku.context_id = 6;
	job.sku.stamp_va = base + 0xc900;
	job.sku.stamp = (g16_render_frame + 1) << 8;
	job.sku.stamp_slot = 0x70;
	job.sku.uuid = 0x16000001 + g16_render_frame * 2;
	/* Reuse the live, resident shared USC pool after compute has retired. */
	g16_render_put(job.sku.uma, g16_initdata_live_barrier_va + 0xe00, 8);
	g16_render_put(job.sku.uma + 0x18, GPU(0x60000), 8);
	ret = g16_render_pack_sku(work + 0x8000, 0x400, G16_RENDER_TA, &job.sku);
	if (ret < 0)
		goto out;
	ret = g16_render_pack_job(work, 0x4000, G16_RENDER_TA, &job, ret);
	if (ret)
		goto out;
	count = g16_render_ta_registers(regs, 128, d, G16_RENDER_TA_DESCRIPTOR_BYTES,
				       &platform, job.sku.uuid, &mode);
	if (count < 0) {
		ret = count;
		goto out;
	}
	ret = g16_render_pack_registers(work, 0x4000, G16_RENDER_TA,
		base & ((1ULL << 42) - 1), regs, count);
	if (ret)
		goto out;
	g16_render_put(work + 0x760, base + g16_render_tpc_offset(), 8);
	g16_render_put(work + 0x768, geometry.tpc_bytes, 8);
	/* Firmware reloads this mirror when resuming the preempted TA job. */
	g16_render_put(work + 0x7a8, TILER(0x40000), 8);
	g16_render_put(work + 0x8fe, g16_initdata_live_barrier_va + 0xe00, 8);
	g16_render_put(work + 0x90f, base + 0x60000, 8);
	g16_render_put(work + 0x830, mode, 8);
	/* Firmware retains this notifier on its intrusive queue list (+94/+98/+a0).
	 * Clearing it after frame zero corrupts the list (25G83 PC 2433c).
	 */
	if (!g16_render_frame) {
		memset(notifier, 0, sizeof(*notifier));
		/* Field offsets match the same G16 notifier used by the compute gate. */
		g16_render_put((u8 *)notifier, base + 0xcb00, 8);
		g16_render_put((u8 *)notifier + 8, 1, 4);
		g16_render_put((u8 *)notifier + 0x10, 50, 4);
		g16_render_put((u8 *)notifier + 0x24, 6, 4);
		g16_render_put((u8 *)notifier + 0xb8, 0x16000001, 4);
	}
	pr_info("g16_lab: render: built experimental %ux%u TA registers=%d VDM=%#llx PB=128 pages TPC=%llu bytes\n",
		g16_render_width(), g16_render_height(), count, g16_render_vdm_base(), geometry.tpc_bytes);
#undef GPU
#undef TILER
#undef D
out:
	kfree(regs);
	kfree(d);
	return ret;
}

/* Preserve transient MMU fault banks before firmware acknowledges IRQ 35. */
static void g16_render_observe_launch(void __iomem *sgx)
{
	struct g16_gpu_uat_fault_bank_capture capture = {};
	unsigned long long deadline = ktime_get_ns() + 5000000;
	u64 irq = 0;
	unsigned int i;

	do {
		irq = readq(sgx + J713_SGX_GPU_IRQ_PAGE_OFFSET + J713_SGX_GPU_IRQ_STATUS_OFFSET);
		if (irq & BIT_ULL(35)) {
			capture.selector_before = readq(sgx + J713_SGX_GPU_FAULT_BANK_OFFSET);
			for (i = 0; i < 8; i++) {
				writeq(i, sgx + J713_SGX_GPU_FAULT_BANK_OFFSET);
				capture.info[i] = readq(sgx + J713_SGX_GPU_FAULT_BANK_OFFSET + J713_SGX_GPU_FAULT_INFO_OFFSET);
				capture.address_register[i] = readq(sgx + J713_SGX_GPU_FAULT_BANK_OFFSET + J713_SGX_GPU_FAULT_ADDR_OFFSET);
			}
			writeq(capture.selector_before, sgx + J713_SGX_GPU_FAULT_BANK_OFFSET);
			capture.captured = true;
			break;
		}
		udelay(1);
	} while (ktime_get_ns() < deadline);
	pr_info("g16_lab: render: launch irq=%#llx fault-captured=%u\n",
		irq, capture.captured);
	if (capture.captured)
		for (i = 0; i < 8; i++) {
			u64 info = capture.info[i];

			pr_info("g16_lab: render: fault bank=%u info=%#llx reason=%llu read=%u unit=%#llx(%s) context=%llu VA=%#llx\n",
				i, info, (u64)FIELD_GET(J713_SGX_GPU_FAULT_INFO_REASON, info),
				!!(info & J713_SGX_GPU_FAULT_INFO_READ),
				(u64)FIELD_GET(J713_SGX_GPU_FAULT_INFO_UNIT, info),
				g16_lab_uat_fault_unit_name(FIELD_GET(J713_SGX_GPU_FAULT_INFO_UNIT, info)),
				(u64)FIELD_GET(J713_SGX_GPU_FAULT_INFO_CONTEXT, info),
				capture.address_register[i] << 6);
		}
}

static int g16_render_test_ta(void __iomem *asc, u64 crashlog_iova,
			    unsigned int crashlog_pages)
{
	u8 *work = g16_render_graph;
	u8 *queue = work + G16_RENDER_QUEUE(0);
	u8 *state = work + G16_RENDER_QUEUE_STATE(0);
	u8 *pipe = work + G16_RENDER_PIPE_RING(0) + ((g16_render_frame + 1) % J713_INITDATA_PIPE_ENTRY_COUNT) * J713_INITDATA_PIPE_ENTRY_SIZE;
	u32 end = (g16_render_frame + 3) % J713_INITDATA_QUEUE_RING_COUNT, stamp = (g16_render_frame + 1) << 8;
	u8 *pipe_state = work + G16_RENDER_PIPE_STATE(0);
	unsigned long deadline;
	u64 timestamp, message;
	u32 endpoint;
	void __iomem *sgx = NULL;
	int ret;

	ret = g16_render_build_ta();
	if (ret)
		return ret;
	if (!g16_render_frame)
		g16_render_put(work + G16_RENDER_QUEUE_RING(0) + 8,
			G16_RENDER_GRAPH_VA + G16_RENDER_INIT_PB, 8);
	g16_render_put(work + G16_RENDER_QUEUE_RING(0) + ((end + J713_INITDATA_QUEUE_RING_COUNT - 1) % J713_INITDATA_QUEUE_RING_COUNT) * 8, G16_RENDER_GRAPH_VA, 8);
	g16_lab_dcache_clean(work, G16_RENDER_GRAPH_BYTES);
	dma_wmb();
	g16_render_put(state + J713_QUEUE_RING_CPU_WPTR_OFFSET, end, 4);
	g16_lab_dcache_clean(state, J713_INITDATA_QUEUE_RING_STATE_SIZE);
	asm volatile("mrs %0, cntpct_el0" : "=r"(timestamp));
	g16_render_put(pipe, timestamp, 8);
	g16_render_put(pipe + 8, G16_RENDER_GRAPH_VA + G16_RENDER_QUEUE(0), 8);
	g16_render_put(pipe + 0x14, end, 2);
	pipe[J713_PIPE_ENTRY_SCHEDULER_SLOT_OFFSET] = 0x70;
	g16_lab_dcache_clean(pipe, J713_INITDATA_PIPE_ENTRY_SIZE);
	dma_wmb();
	g16_render_put(pipe_state + J713_PIPE_PRODUCER_OFFSET, (g16_render_frame + 2) % J713_INITDATA_PIPE_ENTRY_COUNT, 4);
	g16_lab_dcache_clean(pipe_state, J713_INITDATA_PIPE_STATE_SIZE);
	dma_wmb();
	g16_firmware_recovery_observed = false;
	if (render_fault_observe) {
		sgx = ioremap(J713_SGX_MAIN_BASE, SZ_128K);
		if (!sgx)
			return -ENOMEM;
	}
	pr_info("g16_lab: render: kicking TA, direct MMIO observer=%u\n", render_fault_observe);
	ret = g16_asc_send(asc, J713_PIPE_KICK_TA_GROUP0, 0x21);
	if (ret) {
		if (sgx)
			iounmap(sgx);
		return ret;
	}
	if (sgx) {
		g16_render_observe_launch(sgx);
		iounmap(sgx);
	}
	pr_info("g16_lab: render: submitted InitPB + captured vertex/TA job; awaiting GPU-written tiling data\n");
	deadline = jiffies + msecs_to_jiffies(1500);
	do {
		ret = g16_lab_drain_statistics_ring();
		if (ret || g16_firmware_recovery_observed) {
			ret = ret ? ret : -EIO;
			break;
		}
		g16_lab_dcache_invalidate(work + 0xc900, 8);
		g16_lab_dcache_invalidate(queue, J713_INITDATA_QUEUE_INFO_SIZE);
		g16_lab_dcache_invalidate(state, J713_INITDATA_QUEUE_RING_STATE_SIZE);
		if (g16_render_get(work + 0xc900, 4) == stamp &&
		    g16_render_get(state + J713_QUEUE_RING_GPU_RPTR_OFFSET, 4) == end &&
		    !g16_render_get(queue + J713_QUEUE_BUSY_OFFSET, 4)) {
			unsigned int i;

			pr_info("g16_lab: render: TA completion stamp=%#x ring=%u busy=0; snapshotting issued state\n", stamp, end);
			if (g16_drm_active_job)
				return 0;
			g16_lab_dump_gpu_engine_issue_registers();
			for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
				struct g16_render_resource *r = &g16_render_resources[i];

				if (r->va != 0x100000a8000ULL)
					continue;
				g16_lab_dcache_invalidate(r->data, r->size);
				pr_info("g16_lab: render: vertex execution marker=%#llx expected=0x56535441\n",
					g16_render_get(r->data, 4));
				pr_info("g16_lab: render: vertex loader marker=%#llx expected=0x45534c31\n",
					g16_render_get((u8 *)r->data + 4, 4));
			}
			g16_lab_dcache_invalidate(g16_render_pool, G16_RENDER_POOL_BYTES);
			g16_lab_dcache_invalidate(work + 0x28000, 0x1000);
			if (!memchr_inv(g16_render_pool, 0, G16_RENDER_POOL_BYTES) ||
			    !memchr_inv(work + 0x28000, 0, 0x1000)) {
				pr_err("g16_lab: render: TA retired without expected pool/tilemap writes\n");
				return -ENODATA;
			}
			pr_info("g16_lab: render: TA retired with GPU-written pool/tilemap; fragment pixel proof still required\n");
			return 0;
		}
		ret = g16_asc_recv_timeout(asc, &message, &endpoint, 1000);
		if (!ret && endpoint == 1 &&
		    FIELD_GET(RTKIT_MGMT_TYPE, message) == RTKIT_BUFFER_REQUEST &&
		    crashlog_iova && FIELD_GET(RTKIT_BUFFER_IOVA, message) == crashlog_iova) {
			g16_lab_capture_crashlog(crashlog_iova, crashlog_pages);
			ret = -EIO;
			break;
		}
		if (ret && ret != -ETIMEDOUT)
			break;
	} while (time_before(jiffies, deadline));
	g16_lab_log_fault_generation_state(6, "render-TA completion failure");
	g16_lab_dcache_invalidate(work, G16_RENDER_GRAPH_BYTES);
	pr_err("g16_lab: render: TA failed ret=%d stamp=%#llx pipe=%llu/%llu q=%llu/%llu/%llu ring=%llu busy=%llu\n",
		ret, g16_render_get(work + 0xc900, 4), g16_render_get(pipe_state, 4),
		g16_render_get(pipe_state + J713_PIPE_CFI_OFFSET, 4),
		g16_render_get(queue + J713_QUEUE_READER_OFFSET, 4),
		g16_render_get(queue + J713_QUEUE_CFI_OFFSET, 4),
		g16_render_get(queue + J713_QUEUE_PRODUCER_OFFSET, 4),
		g16_render_get(state + J713_QUEUE_RING_GPU_RPTR_OFFSET, 4),
		g16_render_get(queue + J713_QUEUE_BUSY_OFFSET, 4));
	return ret ? ret : -ETIMEDOUT;
}
#endif
