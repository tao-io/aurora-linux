/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_SUBMIT_H
#define G16_DRM_SUBMIT_H

/* Initial Asahi wire submission backend: one root-owned VM/queue per file,
 * serialized render passes, binary syncobjs, and bounded firmware lifetime.
 * Unsupported command fields fail explicitly. No GET_PARAMS admission to
 * Honeykrisp is advertised until its complete initialization contract works.
 */
static int g16_drm_queue_create_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct drm_asahi_queue_create *args = data;
	struct g16_drm_file *priv = file->driver_priv;
	int ret = 0;

	if (!render_drm_submit)
		return -EOPNOTSUPP;
	if (args->flags || args->priority != DRM_ASAHI_PRIORITY_MEDIUM ||
	    args->usc_exec_base != 0x10000000000ULL)
		return -EINVAL;
	mutex_lock(&priv->lock);
	if (!priv->vm || priv->vm->id != args->vm_id)
		ret = -ENOENT;
	else if (priv->queue_id)
		ret = -EBUSY;
	else if (priv->next_queue_id == U32_MAX)
		ret = -ENOSPC;
	else
		args->queue_id = priv->queue_id = ++priv->next_queue_id;
	mutex_unlock(&priv->lock);
	return ret;
}

static int g16_drm_queue_destroy_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	struct drm_asahi_queue_destroy *args = data;
	struct g16_drm_file *priv = file->driver_priv;
	int ret = 0;

	if (args->pad)
		return -EINVAL;
	mutex_lock(&priv->lock);
	if (!args->queue_id || args->queue_id != priv->queue_id)
		ret = -ENOENT;
	else
		priv->queue_id = 0;
	mutex_unlock(&priv->lock);
	return ret;
}

static bool g16_drm_vm_contains(struct g16_drm_vm *vm, u64 va, u64 size)
{
	struct g16_drm_mapping *m;

	list_for_each_entry(m, &vm->mappings, link)
		if (va >= m->va && va - m->va < m->size &&
		    size <= m->size - (va - m->va) &&
		    m->pte_flags == J713_UAT_USER_CAPTURE_GPU_EXEC_SHARED_FLAGS)
			return true;
	return false;
}

static int g16_drm_job_prepare(struct g16_drm_job *job)
{
	struct drm_asahi_cmd_render *r = &job->render;
	struct drm_asahi_cmd_render supported = {
		.vdm_ctrl_stream_base = r->vdm_ctrl_stream_base,
		.width_px = 256, .height_px = 256, .layers = 1,
		.utile_width_px = 32, .utile_height_px = 32,
		.samples = 1, .sample_size_B = 4,
		.bg.usc = r->bg.usc, .eot.usc = r->eot.usc,
	};
	struct g16_drm_mapping *m;
	u64 graph = G16_RENDER_GRAPH_VA & (BIT_ULL(42) - 1);
	int ret;

	/* Start with the qualified geometry and register allocation. Commands
	 * select their own streams/programs; no fixture bytes are copied here.
	 */
	if (memcmp(r, &supported, sizeof(*r)))
		return -EOPNOTSUPP;
	if ((r->vdm_ctrl_stream_base & 3) ||
	    r->vdm_ctrl_stream_base < G16_RENDER_COMPACT_BASE + PAGE_SIZE ||
	    r->vdm_ctrl_stream_base >= G16_RENDER_COMPACT_BASE + BIT_ULL(32) ||
	    !r->bg.usc || !r->eot.usc || (r->bg.usc | r->eot.usc) & 3)
		return -EINVAL;
	if (!g16_drm_vm_contains(job->vm, r->vdm_ctrl_stream_base, 4) ||
	    !g16_drm_vm_contains(job->vm, 0x10000000000ULL + r->bg.usc, 4) ||
	    !g16_drm_vm_contains(job->vm, 0x10000000000ULL + r->eot.usc, 4))
		return -EFAULT;
	if (job->vm->kernel_start > graph || job->vm->kernel_end < graph + G16_RENDER_GRAPH_BYTES)
		return -EINVAL;
	ret = g16_render_uat_map(&job->uat, graph, g16_render_graph,
		G16_RENDER_GRAPH_BYTES, J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS);
	if (!ret)
		ret = g16_render_uat_map(&job->uat, G16_RENDER_POOL_VA, g16_render_pool,
			G16_RENDER_POOL_BYTES, J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS);
	if (ret)
		return ret;
	list_for_each_entry(m, &job->vm->mappings, link) {
		void *cpu = (u8 *)m->map.vaddr + m->offset;

		/* Root 7 is borrowed from the firmware owner. Neither its pages nor
		 * the driver's pool/graph may be shadowed by a client mapping.
		 */
		if (m->va < BIT_ULL(39) && m->va + m->size > 7 * BIT_ULL(36))
			return -EINVAL;
		ret = g16_render_uat_map(&job->uat, m->va, cpu, m->size, m->pte_flags);
		if (!ret)
			ret = g16_render_uat_verify(&job->uat, m->va, cpu, m->size, m->pte_flags);
		if (ret)
			return ret;
	}
	return 0;
}

static int g16_drm_job_switch_root(struct g16_drm_job *job, bool enter)
{
	struct g16_uat_context_slot *slot = g16_initdata_live_uat_kernel_context + 6;
	u64 original = page_to_phys(g16_render_user_uat.tables[0]) |
		J713_UAT_TTBR_ASID(6) | J713_UAT_TTBR_VALID;
	u64 client = page_to_phys(job->uat.tables[0]) |
		J713_UAT_TTBR_ASID(6) | J713_UAT_TTBR_VALID;
	u64 expected = enter ? original : client, next = enter ? client : original;
	int ret;

	ret = g16_lab_uat_handoff_lock(g16_initdata_live_uat_handoff);
	if (ret)
		return ret;
	g16_lab_dcache_invalidate(slot, sizeof(*slot));
	if (READ_ONCE(slot->ttbr0) != expected) {
		g16_lab_uat_handoff_unlock(g16_initdata_live_uat_handoff);
		return -EIO;
	}
	WRITE_ONCE(slot->ttbr0, next);
	job->published = enter;
	g16_lab_dcache_clean(slot, sizeof(*slot));
	dma_wmb();
	g16_lab_uat_handoff_unlock(g16_initdata_live_uat_handoff);
	g16_lab_uat_tlbi_context(6);
	g16_lab_dcache_invalidate(slot, sizeof(*slot));
	return READ_ONCE(slot->ttbr0) == next ? 0 : -EIO;
}

static int g16_drm_job_enter(struct g16_drm_job *job)
{
	struct g16_drm_mapping *m;
	u64 *root = page_address(job->uat.tables[0]);

	if (!g16_render_context_published || root[7])
		return -EINVAL;
	root[7] = g16_initdata_live_user_root[7];
	list_for_each_entry(m, &job->vm->mappings, link)
		g16_lab_dcache_clean((u8 *)m->map.vaddr + m->offset, m->size);
	g16_render_clean_tree(&job->uat);
	dma_wmb();
	pr_info("g16_lab: DRM submit: publishing %u client mappings, %llu bytes, %u UAT leaves\n",
		job->vm->mapping_count, job->vm->mapping_bytes, job->uat.leaves);
	return g16_drm_job_switch_root(job, true);
}

static int g16_drm_job_leave(struct g16_drm_job *job)
{
	return g16_drm_job_switch_root(job, false);
}

/* Called after both engines retire and the original root is restored, or
 * after the enclosing bootstrap has stopped firmware on error. */
static void g16_drm_job_complete(int result)
{
	struct g16_drm_job *job = g16_drm_active_job;
	struct g16_drm_mapping *m;

	if (!job)
		return;
	list_for_each_entry(m, &job->vm->mappings, link)
		g16_lab_dcache_invalidate((u8 *)m->map.vaddr + m->offset, m->size);
	if (result < 0)
		dma_fence_set_error(job->fence, result);
	dma_fence_signal(job->fence);
	job->result = result;
	g16_drm_active_job = NULL;
	complete(&job->done);
}

static int g16_drm_job_reservations(struct g16_drm_job *job)
{
	struct g16_drm_mapping *m, *prior;
	int ret;
	long waited;

	/* Multiple aliases can refer to one reservation. Wait each old fence
	 * before installing ours, or a later alias could wait on its own job.
	 * Imports and cross-file GEM sharing are not admitted by this frontend.
	 */
	list_for_each_entry(m, &job->vm->mappings, link) {
		waited = dma_resv_wait_timeout(m->obj->resv, DMA_RESV_USAGE_READ,
			true, msecs_to_jiffies(100));
		if (waited <= 0)
			return waited < 0 ? waited : -ETIMEDOUT;
	}
	list_for_each_entry(m, &job->vm->mappings, link) {
		bool duplicate = false;

		list_for_each_entry(prior, &job->vm->mappings, link) {
			if (prior == m)
				break;
			if (prior->obj == m->obj) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		ret = dma_resv_lock_interruptible(m->obj->resv, NULL);
		if (ret)
			return ret;
		ret = dma_resv_reserve_fences(m->obj->resv, 1);
		if (!ret)
			dma_resv_add_fence(m->obj->resv, job->fence, DMA_RESV_USAGE_WRITE);
		dma_resv_unlock(m->obj->resv);
		if (ret)
			return ret;
	}
	return 0;
}

static int g16_drm_submit_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_asahi_submit *args = data;
	struct g16_drm_file *priv = file->driver_priv;
	struct drm_asahi_cmd_header header;
	struct drm_asahi_sync syncs[16];
	struct drm_syncobj *outputs[16] = {};
	struct g16_target_fence *fence;
	struct g16_drm_job *job;
	u32 sequence, i, count;
	void *command;
	int ret;

	if (!render_drm_submit)
		return -EOPNOTSUPP;
	if (args->flags || args->pad || args->cmdbuf_size < sizeof(header) ||
	    args->cmdbuf_size > 512 || args->in_sync_count > 16 ||
	    args->out_sync_count > 16 - args->in_sync_count)
		return -EINVAL;
	count = args->in_sync_count + args->out_sync_count;
	if (copy_from_user(syncs, u64_to_user_ptr(args->syncs), count * sizeof(*syncs)))
		return -EFAULT;
	command = memdup_user(u64_to_user_ptr(args->cmdbuf), args->cmdbuf_size);
	if (IS_ERR(command))
		return PTR_ERR(command);
	memcpy(&header, command, sizeof(header));
	if (header.cmd_type != DRM_ASAHI_CMD_RENDER ||
	    header.size != args->cmdbuf_size - sizeof(header) ||
	    (header.vdm_barrier && header.vdm_barrier != DRM_ASAHI_BARRIER_NONE) ||
	    (header.cdm_barrier && header.cdm_barrier != DRM_ASAHI_BARRIER_NONE)) {
		kfree(command);
		return -EINVAL;
	}
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		kfree(command);
		return -ENOMEM;
	}
	/* Zero-filled shorter payloads and zero-only future extensions, matching
	 * the Asahi command buffer ABI. Copy the snapshot, not user memory twice.
	 */
	ret = 0;
	if (header.size > sizeof(job->render) &&
	    memchr_inv(command + sizeof(header) + sizeof(job->render), 0,
		       header.size - sizeof(job->render)))
		ret = -E2BIG;
	memcpy(&job->render, command + sizeof(header), min_t(size_t, header.size, sizeof(job->render)));
	kfree(command);
	if (ret)
		goto free;
	init_completion(&job->done);
	for (i = 0; i < count; i++) {
		struct dma_fence *dependency;
		long waited;

		if (syncs[i].sync_type != DRM_ASAHI_SYNC_SYNCOBJ || syncs[i].timeline_value) {
			ret = -EOPNOTSUPP;
			goto free;
		}
		if (i >= args->in_sync_count) {
			outputs[i] = drm_syncobj_find(file, syncs[i].handle);
			if (!outputs[i]) {
				ret = -ENOENT;
				goto free;
			}
			continue;
		}
		ret = drm_syncobj_find_fence(file, syncs[i].handle, 0, 0, &dependency);
		if (ret)
			goto free;
		waited = dma_fence_wait_timeout(dependency, true, msecs_to_jiffies(100));
		ret = waited < 0 ? waited : !waited ? -ETIMEDOUT : dma_fence_get_status(dependency);
		dma_fence_put(dependency);
		if (ret < 0)
			goto free;
	}
	mutex_lock(&priv->lock);
	if (!args->queue_id || args->queue_id != priv->queue_id) {
		ret = -ENOENT;
		goto unlock;
	}
	if (!mutex_trylock(&g16_render_service_mutex)) {
		ret = -EBUSY;
		goto unlock;
	}
	job->vm = priv->vm;
	ret = g16_drm_job_prepare(job);
	if (ret)
		goto service_unlock;
	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence) {
		ret = -ENOMEM;
		goto service_unlock;
	}
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &g16_target_fence_ops, &fence->lock,
		dma_fence_context_alloc(1), 1);
	job->fence = &fence->base;
	ret = g16_drm_job_reservations(job);
	if (ret)
		goto service_unlock;
	spin_lock(&g16_render_service_state_lock);
	sequence = g16_render_service_completed + 1;
	if (!g16_render_service_ready || g16_render_service_pending || sequence > render_frames) {
		ret = -ESHUTDOWN;
		spin_unlock(&g16_render_service_state_lock);
		goto service_unlock;
	}
	for (i = args->in_sync_count; i < count; i++)
		drm_syncobj_replace_fence(outputs[i], job->fence);
	g16_drm_active_job = job;
	smp_store_release(&g16_render_service_pending, sequence);
	spin_unlock(&g16_render_service_state_lock);
	/* Once published, even a killed client must wait for retirement/teardown.
	 * The worker bounds each hardware wait and its complete service lifetime.
	 */
	wait_for_completion(&job->done);
	ret = job->result;
service_unlock:
	if (job->fence && !dma_fence_is_signaled(job->fence)) {
		dma_fence_set_error(job->fence, ret ?: -ECANCELED);
		dma_fence_signal(job->fence);
	}
	mutex_unlock(&g16_render_service_mutex);
unlock:
	mutex_unlock(&priv->lock);
free:
	for (i = 0; i < ARRAY_SIZE(outputs); i++)
		if (outputs[i])
			drm_syncobj_put(outputs[i]);
	dma_fence_put(job->fence);
	g16_render_uat_free(&job->uat);
	kfree(job);
	return ret;
}
#endif
