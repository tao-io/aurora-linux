/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "g16_cache_ranges.h"

int main(void)
{
	struct g16_cache_range ranges[64];
	unsigned int trial, i, j, n;

	assert(g16_cache_ranges_merge(ranges, 0) == 0);
	/* High kernel addresses sort as unsigned; aliases and adjacent ranges
	 * coalesce without including the unmapped gap between allocations. */
	ranges[0] = (struct g16_cache_range){ULONG_MAX - 16, ULONG_MAX};
	ranges[1] = (struct g16_cache_range){ULONG_MAX - 64, ULONG_MAX - 48};
	ranges[2] = ranges[0];
	ranges[3] = (struct g16_cache_range){ULONG_MAX - 48, ULONG_MAX - 32};
	qsort(ranges, 4, sizeof(*ranges), g16_cache_range_compare);
	n = g16_cache_ranges_merge(ranges, 4);
	assert(n == 2);
	assert(ranges[0].start == ULONG_MAX - 64 && ranges[0].end == ULONG_MAX - 32);
	assert(ranges[1].start == ULONG_MAX - 16 && ranges[1].end == ULONG_MAX);

	/* Independent byte-coverage oracle checks that no mapped byte is lost
	 * and no gap (potential unrelated dirty data) is added. */
	srand(1);
	for (trial = 0; trial < 2000; trial++) {
		bool before[256] = {}, after[256] = {};
		unsigned int count = rand() % 65;

		for (i = 0; i < count; i++) {
			unsigned int start = rand() % 256;
			unsigned int end = start + rand() % (257 - start);

			ranges[i] = (struct g16_cache_range){start, end};
			for (j = start; j < end; j++)
				before[j] = true;
		}
		qsort(ranges, count, sizeof(*ranges), g16_cache_range_compare);
		n = g16_cache_ranges_merge(ranges, count);
		assert(n <= count);
		for (i = 0; i < n; i++) {
			assert(ranges[i].start < ranges[i].end);
			if (i)
				assert(ranges[i - 1].end < ranges[i].start);
			for (j = ranges[i].start; j < ranges[i].end; j++)
				after[j] = true;
		}
		assert(!memcmp(before, after, sizeof(before)));
		assert(g16_cache_ranges_merge(ranges, n) == n);
	}
	puts("PASS: cache range aliases, gaps, adjacency, high addresses and 2000 coverage cases");
}
