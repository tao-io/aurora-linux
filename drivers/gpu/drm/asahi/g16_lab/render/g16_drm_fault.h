/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_FAULT_H
#define G16_DRM_FAULT_H

struct g16_drm_fault_blob {
	struct list_head link;
	struct debugfs_blob_wrapper blob;
};
static LIST_HEAD(g16_drm_fault_blobs);
static struct dentry *g16_drm_fault_dir;

static void g16_drm_fault_blob_add(const char *name, const void *data, size_t bytes)
{
	struct g16_drm_fault_blob *saved = kzalloc(sizeof(*saved), GFP_KERNEL);

	if (!saved)
		return;
	saved->blob.data = kvmalloc(bytes, GFP_KERNEL);
	if (!saved->blob.data) {
		kfree(saved);
		return;
	}
	memcpy(saved->blob.data, data, bytes);
	saved->blob.size = bytes;
	list_add_tail(&saved->link, &g16_drm_fault_blobs);
	debugfs_create_blob(name, 0400, g16_drm_fault_dir, &saved->blob);
}

/* Called by the sole GPU worker before the failed job releases its VM pins.
 * Retain command/state prefixes for postmortem decoding; this diagnostic
 * truncation has no effect on which resources the GPU may access. */
static void g16_drm_fault_capture(struct g16_drm_job *job)
{
	struct g16_drm_mapping *m;
	unsigned int i;
	char name[128];

	if (g16_drm_fault_dir)
		return;
	g16_drm_fault_dir = debugfs_create_dir("g16_lab_drm_fault", NULL);
	if (IS_ERR_OR_NULL(g16_drm_fault_dir)) {
		g16_drm_fault_dir = NULL;
		return;
	}
	g16_drm_fault_blob_add("render-command.bin", &job->render,
			      sizeof(job->render) + sizeof(job->rsrc_hi));
	list_for_each_entry(m, &job->vm->mappings, link) {
		snprintf(name, sizeof(name), "va-%llx-size-%llx-flags-%llx.bin",
			 m->va, m->size, m->pte_flags);
		g16_drm_fault_blob_add(name, (u8 *)m->map.vaddr + m->offset,
				      min_t(u64, m->size, SZ_128K));
	}
	for (i = 0; i < job->uat.count; i++) {
		snprintf(name, sizeof(name), "uat-%u-pa-%llx.bin", i,
			 (u64)page_to_phys(job->uat.tables[i]));
		g16_drm_fault_blob_add(name, page_address(job->uat.tables[i]), PAGE_SIZE);
	}
	pr_info("g16_lab: retained failed DRM command, UAT and mapping prefixes in debugfs\n");
}

static void g16_drm_fault_release(void)
{
	struct g16_drm_fault_blob *saved, *next;

	debugfs_remove_recursive(g16_drm_fault_dir);
	g16_drm_fault_dir = NULL;
	list_for_each_entry_safe(saved, next, &g16_drm_fault_blobs, link) {
		list_del(&saved->link);
		kvfree(saved->blob.data);
		kfree(saved);
	}
}
#endif
