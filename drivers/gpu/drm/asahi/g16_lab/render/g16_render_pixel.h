/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_PIXEL_H
#define G16_RENDER_PIXEL_H

/* First bounded 3D submission: geometry comes from the preceding TA pass.
 * Captured M4 background/end-of-tile loaders are bound below. Their resource
 * allocation words still need qualification; this is a bounded diagnostic.
 * Retirement alone is deliberately not accepted as a rendered image.
 */
static int g16_render_build_fragment(void)
{
	struct g16_render_geometry g;
	struct g16_render_fragment_platform platform = {
		.accelerator_count_4e4 = 1,
		.disable_spill_registers = 1,
	};
	struct g16_render_job_args job = {};
	struct g16_render_register *regs;
	u8 *d, *work = g16_render_graph + 0x4000;
	u64 base = G16_RENDER_GRAPH_VA, mode;
	int count, bytes, ret;

	d = kzalloc(G16_RENDER_FRAGMENT_DESCRIPTOR_BYTES, GFP_KERNEL);
	regs = kcalloc(128, sizeof(*regs), GFP_KERNEL);
	if (!d || !regs) {
		ret = -ENOMEM;
		goto out;
	}
	ret = g16_render_geometry_single(&g, render_size, render_size, 32, 32, 1);
	if (ret)
		goto out;
#define D(o, v, n) g16_render_put(d + (o), (v), (n))
#define GPU(o) ((base + (o)) & ((1ULL << 42) - 1))
	/* Same-build loadRenderTargetMemoryData uses normal GPU addresses. */
	D(0x41c, 32, 4);
	D(0x420, 32, 4);
	/* HAL200 emitDriverProgramArguments writes common+a8 (raw+378).
	 * copy3DCommonPassthroughData maps it to descriptor+588 / reg15370.
	 * emitEndOfTileArgumentsEarly supplies common+c8 / reg15380.
	 * Resource 35 contains BGO loaders at +0/+240 and EOT at +480.
	 */
	D(0x588, 0x10000158000ULL, 8);
	D(0x6e0, 0x10000158480ULL, 8);
	D(0x7d0, GPU(0x28000), 8);
	D(0x7d8, GPU(0x29000), 8);
	D(0x7e0, GPU(0x29000), 8);
	D(0x7f8, 1, 4);
	D(0x800, g.isp_blocks, 4);
	D(0x804, g.screen, 4);
	D(0x808, g.x_blocks, 4);
	D(0x80c, g.y_blocks, 4);
	D(0x810, g.blocks, 4);
	D(0x820, 1, 4);
	D(0x824, g.region_stride, 4);
	D(0x828, g.tiles, 4);
	D(0x890, 1, 1);
	D(0x891, 1, 1);
	job.notifier_va = base + 0xc000;
	job.shared_stamp_va = base + 0xc808;
	job.pb_aux_va = base + 0x9300;
	job.context_generation = 1;
	job.command_counter = g16_render_frame * 2 + 2;
	job.sku.command_va = base + 0x4000;
	job.sku.sku_va = base + 0x8400;
	job.sku.queue_va = base + G16_RENDER_QUEUE(1);
	job.sku.slot_array_va = base + G16_RENDER_SLOT_ARRAY(1) + 8;
	/* Both engines consume the same pass and parameter-buffer manager. */
	job.sku.pb_slot_va = base + 0x9200;
	job.sku.buffer_info_va = base + 0x9000;
	job.sku.context_id = 6;
	job.sku.stamp_va = base + 0xc908;
	job.sku.stamp = (g16_render_frame + 1) << 8;
	job.sku.stamp_slot = 0x71;
	job.sku.uuid = 0x16000002 + g16_render_frame * 2;
	job.sku.fragment_increment = 1;
	g16_render_put(job.sku.uma, g16_initdata_live_barrier_va + 0xe00, 8);
	g16_render_put(job.sku.uma + 0x18, GPU(0x60100), 8);
	bytes = g16_render_pack_sku(g16_render_graph + 0x8400, 0x400,
				  G16_RENDER_FRAGMENT, &job.sku);
	if (bytes < 0) {
		ret = bytes;
		goto out;
	}
	ret = g16_render_pack_job(work, 0x4000, G16_RENDER_FRAGMENT, &job, bytes);
	if (ret)
		goto out;
	count = g16_render_fragment_registers(regs, 128, d,
		G16_RENDER_FRAGMENT_DESCRIPTOR_BYTES, &platform, job.sku.uuid, &mode);
	if (count < 0) {
		ret = count;
		goto out;
	}
	ret = g16_render_pack_registers(work, 0x4000, G16_RENDER_FRAGMENT,
		GPU(0x4000), regs, count);
	if (ret)
		goto out;
	/* submitBuffer mirrors geometry into the firmware command prefix too. */
	g16_render_put(work + 0x40, GPU(0x28000), 8);
	g16_render_put(work + 0x50, 1, 4);
	g16_render_put(work + 0x54, g.isp_blocks, 4);
	g16_render_put(work + 0x60, g.x_blocks, 4);
	g16_render_put(work + 0x64, g.y_blocks, 4);
	g16_render_put(work + 0x68, g.blocks, 4);
	g16_render_put(work + 0x78, g.tiles, 4);
	/* Same native submitBuffer mirrors used by firmware diagnostics. */
	g16_render_put(work + 0x900, 0x10000158000ULL, 8);
	g16_render_put(work + 0x9fc, 0x10000158480ULL, 8);
	g16_render_put(work + 0xc46, g16_initdata_live_barrier_va + 0xe00, 8);
	g16_render_put(work + 0xc57, base + 0x60100, 8);
	g16_render_put(work + 0x9d8, mode, 8);
	pr_info("g16_lab: render: built %ux%u fragment diagnostic registers=%d mode=%#llx; pixel output not yet verified\n",
		render_size, render_size, count, mode);
#undef GPU
#undef D
out:
	kfree(regs);
	kfree(d);
	return ret;
}

static int g16_render_verify_pixels(void)
{
	unsigned int i, pixel;
	u32 expected = render_generated_shaders ? 0xff0000ff : 0xffbf8040;

	for (i = 0; i < G16_RENDER_RESOURCE_COUNT; i++) {
		struct g16_render_resource *r = &g16_render_resources[i];

		if (r->va != g16_render_target_va() ||
		    r->size < g16_render_target_offset() + render_size * render_size * 4)
			continue;
		g16_lab_dcache_invalidate(r->data, r->size);
		if (memchr_inv(r->data, 0, g16_render_target_offset()) ||
		    memchr_inv(r->data + g16_render_target_offset() + render_size * render_size * 4,
			       0, r->size - g16_render_target_offset() - render_size * render_size * 4)) {
			pr_err("g16_lab: render: frame=%u modified target padding\n", g16_render_frame);
			return -EOVERFLOW;
		}
		for (pixel = 0; pixel < render_size * render_size; pixel++) {
			u32 index = pixel, actual;

			if (render_xy) {
				u32 x = pixel % 256, y = pixel / 256, bit;
				u32 red = DIV_ROUND_CLOSEST(x * 255, 256);
				u32 green = DIV_ROUND_CLOSEST(y * 255, 256);

				/* Uncompressed RGBA8: row-major 64x64 pages, Morton
				 * pixel ordering within each 16 KiB page.
				 */
				index = ((y / 64) * 4 + x / 64) * 4096;
				for (bit = 0; bit < 6; bit++)
					index |= ((x >> bit) & 1) << (2 * bit) |
						 ((y >> bit) & 1) << (2 * bit + 1);
				expected = 0xff000000 | green << 8 | red;
				if (render_triangle) {
					if (x < y || (g16_render_frame & 1))
						expected = 0xff000000 |
							DIV_ROUND_CLOSEST((256 - y) * 255, 256) << 16 |
							red << 8 | DIV_ROUND_CLOSEST((y > x ? y - x : 0) * 255, 256);
					else
						expected = 0; /* fixture preserves the zero background */
				}
			}
			actual = g16_render_get(r->data + g16_render_target_offset() + index * 4, 4);

			if (actual != expected) {
				pr_err("g16_lab: render: pixel %u=%#x expected=%#x\n",
					pixel, actual, expected);
				return -ENODATA;
			}
		}
		if (render_triangle)
			memcpy(g16_render_frames[g16_render_frame].data,
			       r->data + g16_render_target_offset(), 256 * 256 * 4);
		pr_info("g16_lab: render: verified frame=%u pixels=%u stamp=%#x expanded=%u\n",
			g16_render_frame, render_size * render_size, (g16_render_frame + 1) << 8,
			g16_render_frame & 1);
		return 0;
	}
	return -ENOENT;
}

static int g16_render_test_fragment(void __iomem *asc, u64 crashlog_iova,
				    unsigned int crashlog_pages)
{
	u8 *work = g16_render_graph;
	u8 *queue = work + G16_RENDER_QUEUE(1);
	u8 *state = work + G16_RENDER_QUEUE_STATE(1);
	u8 *pipe = work + G16_RENDER_PIPE_RING(1) + (g16_render_frame + 1) * J713_INITDATA_PIPE_ENTRY_SIZE;
	u32 end = g16_render_frame + 2, stamp = (g16_render_frame + 1) << 8;
	u8 *pipe_state = work + G16_RENDER_PIPE_STATE(1);
	unsigned long deadline;
	u64 timestamp, message;
	u32 endpoint;
	int ret;

	ret = g16_render_build_fragment();
	if (ret)
		return ret;
	g16_render_put(work + G16_RENDER_QUEUE_RING(1) + (end - 1) * 8,
		G16_RENDER_GRAPH_VA + 0x4000, 8);
	g16_lab_dcache_clean(work, G16_RENDER_GRAPH_BYTES);
	dma_wmb();
	g16_render_put(state + J713_QUEUE_RING_CPU_WPTR_OFFSET, end, 4);
	g16_lab_dcache_clean(state, J713_INITDATA_QUEUE_RING_STATE_SIZE);
	asm volatile("mrs %0, cntpct_el0" : "=r"(timestamp));
	g16_render_put(pipe, timestamp, 8);
	g16_render_put(pipe + 8, G16_RENDER_GRAPH_VA + G16_RENDER_QUEUE(1), 8);
	g16_render_put(pipe + 0x10, 1, 4);
	g16_render_put(pipe + 0x14, end, 2);
	pipe[J713_PIPE_ENTRY_SCHEDULER_SLOT_OFFSET] = 0x71;
	g16_lab_dcache_clean(pipe, J713_INITDATA_PIPE_ENTRY_SIZE);
	dma_wmb();
	g16_render_put(pipe_state + J713_PIPE_PRODUCER_OFFSET, g16_render_frame + 2, 4);
	g16_lab_dcache_clean(pipe_state, J713_INITDATA_PIPE_STATE_SIZE);
	dma_wmb();
	g16_firmware_recovery_observed = false;
	ret = g16_asc_send(asc, J713_PIPE_KICK_TA_GROUP0 | 1, 0x21);
	if (ret)
		return ret;
	pr_info("g16_lab: render: submitted fragment diagnostic after retired TA\n");
	deadline = jiffies + msecs_to_jiffies(1500);
	do {
		ret = g16_lab_drain_statistics_ring();
		if (ret || g16_firmware_recovery_observed)
			return ret ? ret : -EIO;
		g16_lab_dcache_invalidate(work + 0xc908, 8);
		g16_lab_dcache_invalidate(queue, J713_INITDATA_QUEUE_INFO_SIZE);
		g16_lab_dcache_invalidate(state, J713_INITDATA_QUEUE_RING_STATE_SIZE);
		if (g16_render_get(work + 0xc908, 4) == stamp &&
		    g16_render_get(state + J713_QUEUE_RING_GPU_RPTR_OFFSET, 4) == end &&
		    !g16_render_get(queue + J713_QUEUE_BUSY_OFFSET, 4)) {
			pr_info("g16_lab: render: fragment diagnostic retired stamp=%#x ring=%u; pixel verification still required\n", stamp, end);
			return g16_render_verify_pixels();
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
	g16_lab_log_fault_generation_state(6, "render-fragment completion failure");
	pr_err("g16_lab: render: fragment timed out stamp=%#llx ring=%llu busy=%llu\n",
		g16_render_get(work + 0xc908, 4),
		g16_render_get(state + J713_QUEUE_RING_GPU_RPTR_OFFSET, 4),
		g16_render_get(queue + J713_QUEUE_BUSY_OFFSET, 4));
	return -ETIMEDOUT;
}
#endif
