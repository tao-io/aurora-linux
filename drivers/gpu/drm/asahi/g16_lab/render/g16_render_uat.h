/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_UAT_H
#define G16_RENDER_UAT_H

/* Private, unpublished 42-bit / 16 KiB UAT trees. Own every intermediate page. */
#define G16_RENDER_UAT_TABLE_LIMIT 64
struct g16_render_uat {
	struct page *tables[G16_RENDER_UAT_TABLE_LIMIT];
	unsigned int count;
	unsigned int leaves;
};

static void g16_render_uat_free(struct g16_render_uat *u)
{
	while (u->count)
		__free_page(u->tables[--u->count]);
	memset(u, 0, sizeof(*u));
}

static u64 *g16_render_uat_new_table(struct g16_render_uat *u)
{
	struct page *p;

	if (u->count == G16_RENDER_UAT_TABLE_LIMIT)
		return NULL;
	p = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!p)
		return NULL;
	if (page_to_phys(p) & ~J713_UAT_PHYS_MASK) {
		__free_page(p);
		return NULL;
	}
	u->tables[u->count++] = p;
	return page_address(p);
}

static u64 *g16_render_uat_owned_table(struct g16_render_uat *u, u64 entry)
{
	unsigned int i;

	if ((entry & ~J713_UAT_PHYS_MASK) != J713_UAT_TABLE_DESC_FLAGS)
		return NULL;
	for (i = 0; i < u->count; i++)
		if (page_to_phys(u->tables[i]) == (entry & J713_UAT_PHYS_MASK))
			return page_address(u->tables[i]);
	return NULL;
}

/* Clone a validated, unpublished VM tree. Intermediate pointers must point
 * to the clone; leaf mappings retain their pinned GEM pages and permissions.
 * The caller holds the VM lock and publishes only after adding private ranges.
 */
static int g16_render_uat_clone_links(struct g16_render_uat *dst,
				    struct g16_render_uat *src,
				    unsigned int table_index, unsigned int level)
{
	u64 *old = page_address(src->tables[table_index]);
	u64 *next = page_address(dst->tables[table_index]);
	unsigned int i, j, entries = level ? PAGE_SIZE / sizeof(u64) : 64;
	int ret;

	for (i = 0; i < entries; i++) {
		if (!old[i])
			continue;
		if (!g16_render_uat_owned_table(src, old[i]))
			return -EINVAL;
		for (j = 0; j < src->count; j++)
			if (page_to_phys(src->tables[j]) == (old[i] & J713_UAT_PHYS_MASK))
				break;
		if (j == src->count || j == table_index)
			return -EINVAL;
		next[i] = page_to_phys(dst->tables[j]) | J713_UAT_TABLE_DESC_FLAGS;
		if (!level) {
			ret = g16_render_uat_clone_links(dst, src, j, 1);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int g16_render_uat_clone(struct g16_render_uat *dst,
				struct g16_render_uat *src)
{
	unsigned int i;
	int ret;

	if (dst->count || src->count > G16_RENDER_UAT_TABLE_LIMIT)
		return -EINVAL;
	if (!src->count)
		return 0;
	for (i = 0; i < src->count; i++) {
		u64 *table = g16_render_uat_new_table(dst);

		if (!table) {
			ret = -ENOMEM;
			goto fail;
		}
		memcpy(table, page_address(src->tables[i]), PAGE_SIZE);
	}
	ret = g16_render_uat_clone_links(dst, src, 0, 0);
	if (ret)
		goto fail;
	dst->leaves = src->leaves;
	return 0;
fail:
	g16_render_uat_free(dst);
	return ret;
}

static u64 *g16_render_uat_leaf(struct g16_render_uat *u, u64 va, bool create)
{
	const unsigned int shifts[] = {36, 25};
	u64 *table;
	unsigned int level;

	if (va >= (1ULL << 42))
		return NULL;
	if (!u->count && (!create || !g16_render_uat_new_table(u)))
		return NULL;
	table = page_address(u->tables[0]);
	for (level = 0; level < ARRAY_SIZE(shifts); level++) {
		unsigned int index = (va >> shifts[level]) & (level ? 0x7ff : 0x3f);
		u64 *child;

		if (!table[index] && create) {
			child = g16_render_uat_new_table(u);
			if (!child)
				return NULL;
			table[index] = page_to_phys(u->tables[u->count - 1]) |
				J713_UAT_TABLE_DESC_FLAGS;
		} else {
			child = g16_render_uat_owned_table(u, table[index]);
			if (!child)
				return NULL;
		}
		table = child;
	}
	return table + ((va >> 14) & 0x7ff);
}

static int g16_render_uat_map(struct g16_render_uat *u, u64 va,
			      void *data, size_t bytes, u64 flags)
{
	size_t offset;

	if (!data || !bytes || (bytes & 0x3fff) || (va & 0x3fff) ||
	    ((unsigned long)data & 0x3fff) || va >= (1ULL << 42) ||
	    bytes > (1ULL << 42) - va || (flags & J713_UAT_PHYS_MASK) ||
	    !(flags & J713_UAT_PTE_VALID))
		return -EINVAL;
	/* A failed map is discarded with its entire private tree by the caller. */
	for (offset = 0; offset < bytes; offset += PAGE_SIZE) {
		struct page *p = vmalloc_to_page((u8 *)data + offset);
		u64 *leaf;

		if (!p || (page_to_phys(p) & ~J713_UAT_PHYS_MASK))
			return -ERANGE;
		leaf = g16_render_uat_leaf(u, va + offset, true);
		if (!leaf)
			return -ENOMEM;
		if (*leaf)
			return -EEXIST;
		*leaf = page_to_phys(p) | flags;
		u->leaves++;
	}
	return 0;
}

static int g16_render_uat_verify(struct g16_render_uat *u, u64 va,
				 void *data, size_t bytes, u64 flags)
{
	size_t offset;

	for (offset = 0; offset < bytes; offset += PAGE_SIZE) {
		struct page *p = vmalloc_to_page((u8 *)data + offset);
		u64 *leaf = g16_render_uat_leaf(u, va + offset, false);

		if (!p || !leaf || *leaf != (page_to_phys(p) | flags))
			return -EIO;
	}
	return 0;
}
#endif
