/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_CACHE_RANGES_H
#define G16_CACHE_RANGES_H

/* CPU virtual ranges, independent of the GPU's executable/read-write aliases. */
struct g16_cache_range {
	unsigned long start, end;
};

static inline int g16_cache_range_compare(const void *a, const void *b)
{
	const struct g16_cache_range *x = a, *y = b;

	return (x->start > y->start) - (x->start < y->start);
}

/* Input is sorted by start. Merge only overlapping or adjacent intervals;
 * a gap may contain unrelated dirty CPU data and must never be invalidated.
 */
static inline unsigned int g16_cache_ranges_merge(struct g16_cache_range *ranges,
						unsigned int count)
{
	unsigned int i, used = 0;

	for (i = 0; i < count; i++) {
		struct g16_cache_range r = ranges[i];

		if (r.start == r.end)
			continue;
		if (used && r.start <= ranges[used - 1].end) {
			if (r.end > ranges[used - 1].end)
				ranges[used - 1].end = r.end;
		} else {
			ranges[used++] = r;
		}
	}
	return used;
}
#endif
