/* SPDX-License-Identifier: GPL-2.0-only */
/* Userspace model of page allocation; exercise the actual UAT implementation. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
typedef uint8_t u8;
#define PAGE_SIZE 16384U
#define GFP_KERNEL 0
#define __GFP_ZERO 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define J713_UAT_PHYS_MASK (((1ULL << 42) - 1) & ~(PAGE_SIZE - 1ULL))
#define J713_UAT_PTE_VALID 1ULL
#define J713_UAT_TABLE_DESC_FLAGS 3ULL
struct page { void *data; u64 phys; };
static struct page *pages[1024];
static unsigned int serial, live;
static int fail_after = -1;
static struct page *alloc_page(int flags)
{
	struct page *p;
	(void)flags;
	if (!fail_after)
		return NULL;
	if (fail_after > 0)
		fail_after--;
	assert(serial + 1 < ARRAY_SIZE(pages));
	p = calloc(1, sizeof(*p));
	assert(p);
	p->data = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	assert(p->data);
	memset(p->data, 0, PAGE_SIZE);
	p->phys = ++serial * (u64)PAGE_SIZE;
	pages[serial] = p;
	live++;
	return p;
}
static void __free_page(struct page *p)
{
	pages[p->phys / PAGE_SIZE] = NULL;
	free(p->data);
	free(p);
	live--;
}
static void *page_address(struct page *p) { return p->data; }
static u64 page_to_phys(struct page *p) { return p->phys; }
static struct page *vmalloc_to_page(void *ptr)
{
	unsigned int i;
	for (i = 1; i <= serial; i++)
		if (pages[i] && pages[i]->data == ptr)
			return pages[i];
	return NULL;
}
#include "g16_render_uat.h"

int main(void)
{
	struct g16_render_uat src = {}, dst = {};
	struct page *data = alloc_page(0);
	const u64 flags = (1ULL << 53) | 0x403;
	const u64 va[] = {0x4000, 0x2010000, 0x1000040000ULL, 0x31000000000ULL};
	unsigned int i, count, baseline;
	u64 old;

	assert(g16_render_uat_clone(&dst, &src) == 0 && !dst.count);
	for (i = 0; i < ARRAY_SIZE(va); i++)
		assert(!g16_render_uat_map(&src, va[i], data->data, PAGE_SIZE, flags));
	baseline = live;
	count = src.count;
	for (i = 0; i < count; i++) {
		fail_after = i;
		assert(g16_render_uat_clone(&dst, &src) == -ENOMEM);
		assert(!dst.count && live == baseline && src.count == count);
	}
	fail_after = -1;
	assert(!g16_render_uat_clone(&dst, &src));
	assert(dst.count == count && dst.leaves == ARRAY_SIZE(va));
	for (i = 0; i < ARRAY_SIZE(va); i++) {
		assert(!g16_render_uat_verify(&dst, va[i], data->data, PAGE_SIZE, flags));
		assert(g16_render_uat_leaf(&src, va[i], false) !=
		       g16_render_uat_leaf(&dst, va[i], false));
	}
	*g16_render_uat_leaf(&dst, va[0], false) = 0;
	assert(!g16_render_uat_verify(&src, va[0], data->data, PAGE_SIZE, flags));
	assert(g16_render_uat_clone(&dst, &src) == -EINVAL);
	g16_render_uat_free(&dst);
	old = ((u64 *)page_address(src.tables[0]))[0];
	((u64 *)page_address(src.tables[0]))[0] = 0x3fffffc003ULL;
	assert(g16_render_uat_clone(&dst, &src) == -EINVAL);
	assert(!dst.count && live == baseline);
	((u64 *)page_address(src.tables[0]))[0] = old;
	assert(!g16_render_uat_clone(&dst, &src));
	g16_render_uat_free(&src);
	for (i = 0; i < ARRAY_SIZE(va); i++)
		assert(!g16_render_uat_verify(&dst, va[i], data->data, PAGE_SIZE, flags));
	assert(!g16_render_uat_leaf(&dst, 0x4000000, false));
	g16_render_uat_free(&dst);
	__free_page(data);
	assert(!live);
	puts("UAT clone: independent tables, retained leaves, allocation failure and invalid links pass");
}
