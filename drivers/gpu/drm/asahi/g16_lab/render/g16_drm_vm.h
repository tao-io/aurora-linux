/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_VM_H
#define G16_DRM_VM_H

/* Initially private, unpublished UAT roots. Bind takes its own GEM reference
 * and vmap pin; dropping a userspace handle cannot invalidate a mapped page.
 * Rebuild before replacing the root so errors preserve all earlier bindings.
 */
struct g16_drm_mapping {
	struct list_head link;
	struct drm_gem_object *obj;
	struct iosys_map map;
	u64 va, offset, size, pte_flags;
};

struct g16_drm_vm {
	u32 id;
	u32 mapping_count;
	u64 mapping_bytes;
	u64 kernel_start, kernel_end;
	struct list_head mappings;
	struct g16_render_uat uat;
};

static void g16_drm_mapping_free(struct g16_drm_mapping *mapping)
{
	drm_gem_vunmap(mapping->obj, &mapping->map);
	drm_gem_object_put(mapping->obj);
	kfree(mapping);
}

static void g16_drm_vm_free(struct g16_drm_vm *vm)
{
	struct g16_drm_mapping *mapping, *next;

	if (!vm)
		return;
	g16_render_uat_free(&vm->uat);
	list_for_each_entry_safe(mapping, next, &vm->mappings, link) {
		list_del(&mapping->link);
		g16_drm_mapping_free(mapping);
	}
	kfree(vm);
}

static int g16_drm_vm_rebuild(struct g16_drm_vm *vm)
{
	struct g16_render_uat *next;
	struct g16_drm_mapping *mapping;
	int ret = 0;

	next = kzalloc(sizeof(*next), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	list_for_each_entry(mapping, &vm->mappings, link) {
		void *data = (u8 *)mapping->map.vaddr + mapping->offset;

		ret = g16_render_uat_map(next, mapping->va, data, mapping->size, mapping->pte_flags);
		if (!ret)
			ret = g16_render_uat_verify(next, mapping->va, data, mapping->size, mapping->pte_flags);
		if (ret)
			break;
	}
	if (!ret) {
		g16_render_uat_free(&vm->uat);
		vm->uat = *next;
		memset(next, 0, sizeof(*next));
	}
	g16_render_uat_free(next);
	kfree(next);
	return ret;
}

static int g16_drm_vm_bind_one(struct g16_drm_vm *vm, struct drm_file *file,
				   const struct g16_drm_bind_op *op)
{
	struct g16_drm_mapping *mapping, *found = NULL;
	struct drm_gem_object *obj;
	u64 end;
	int ret;

	if (!op->range || (op->range | op->offset | op->addr) & (PAGE_SIZE - 1) ||
	    op->addr < PAGE_SIZE || op->addr >= BIT_ULL(42) || op->range > BIT_ULL(42) - op->addr)
		return -EINVAL;
	end = op->addr + op->range;
	if (op->addr < vm->kernel_end && vm->kernel_start < end)
		return -EINVAL;
	list_for_each_entry(mapping, &vm->mappings, link) {
		if (op->addr < mapping->va + mapping->size && mapping->va < end) {
			found = mapping;
			break;
		}
	}
	if (op->flags == G16_BIND_UNBIND) {
		/* Exact mapping removal for this initial bounded frontend. */
		if (op->handle || op->offset)
			return -EINVAL;
		if (!found || found->va != op->addr || found->size != op->range)
			return -ENOENT;
		list_del(&found->link);
		ret = g16_drm_vm_rebuild(vm);
		if (ret) {
			list_add_tail(&found->link, &vm->mappings);
			return ret;
		}
		vm->mapping_count--;
		vm->mapping_bytes -= found->size;
		g16_drm_mapping_free(found);
		return 0;
	}
	if (!(op->flags & G16_BIND_READ) || op->flags & ~(G16_BIND_READ | G16_BIND_WRITE))
		return -EINVAL;
	obj = drm_gem_object_lookup(file, op->handle);
	if (!obj)
		return -ENOENT;
	if (g16_drm_to_gem(obj)->private_vm_id &&
	    g16_drm_to_gem(obj)->private_vm_id != vm->id) {
		ret = -EINVAL;
		goto put;
	}
	if (op->offset >= obj->size || op->range > obj->size - op->offset) {
		ret = -EINVAL;
		goto put;
	}
	/* Honeykrisp rebinds a cached allocation's existing read-only shadow.
	 * Identical bindings are idempotent; conflicting overlaps remain errors. */
	if (found) {
		u64 flags = op->flags & G16_BIND_WRITE ? J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS :
			J713_UAT_USER_CAPTURE_GPU_EXEC_SHARED_FLAGS;

		ret = found->obj == obj && found->va == op->addr && found->size == op->range &&
		      found->offset == op->offset && found->pte_flags == flags ? 0 : -EEXIST;
		goto put;
	}
	if (vm->mapping_count >= (render_drm_submit ? 1024 : 128) || op->range > SZ_256M - vm->mapping_bytes) {
		ret = -ENOSPC;
		goto put;
	}
	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping) {
		ret = -ENOMEM;
		goto put;
	}
	mapping->obj = obj;
	ret = drm_gem_vmap(obj, &mapping->map);
	if (ret) {
		kfree(mapping);
		goto put;
	}
	if (mapping->map.is_iomem || !is_vmalloc_addr(mapping->map.vaddr)) {
		g16_drm_mapping_free(mapping);
		return -EINVAL;
	}
	mapping->va = op->addr;
	mapping->offset = op->offset;
	mapping->size = op->range;
	mapping->pte_flags = op->flags & G16_BIND_WRITE ?
		J713_UAT_USER_GPU_RW_NX_SHARED_FLAGS : J713_UAT_USER_CAPTURE_GPU_EXEC_SHARED_FLAGS;
	list_add_tail(&mapping->link, &vm->mappings);
	ret = g16_drm_vm_rebuild(vm);
	if (ret) {
		list_del(&mapping->link);
		g16_drm_mapping_free(mapping);
		return ret;
	}
	vm->mapping_count++;
	vm->mapping_bytes += mapping->size;
	return 0;
put:
	drm_gem_object_put(obj);
	return ret;
}
#endif
