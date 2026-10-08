// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../drivers/spi/spi-apple-qspi-policy.h"
int main(void)
{
	uint64_t a;
	unsigned int op, n, s1, s2, s3;
	for (a = 0; a < 0x1000000; a++) {
		int erase = apple_qspi_mutation_allowed(0x20, 3, a, 0, 0);
		assert(erase ==
		       (a >= 0x700000 && a <= 0x7ff000 && !(a & 4095)));
		assert(apple_qspi_mutation_allowed(0x02, 3, a, 1, 1) ==
		       (a >= 0x700000 && a < 0x800000));
	}
	for (a = 0x6fff00; a <= 0x800100; a += 1)
		for (n = 0; n <= 257; n++) {
			int accepted =
				apple_qspi_mutation_allowed(0x02, 3, a, n, 1);
			assert(accepted ==
			       (n > 0 && n <= 256 && a >= 0x700000 &&
				a < 0x800000 && a + n <= 0x800000 &&
				(a & 255) + n <= 256));
		}
	for (op = 0; op < 256; op++) {
		if (op != 2 && op != 0x20)
			assert(!apple_qspi_mutation_allowed(op, 3, 0x700000, 16,
							    1));
		if (op != 6 && op != 4)
			assert(!apple_qspi_mutation_allowed(op, 0, 0, 0, 0));
	}
	for (n = 0; n <= 8; n++)
		if (n != 3) {
			assert(!apple_qspi_mutation_allowed(2, n, 0x700000, 1,
							    1));
			assert(!apple_qspi_mutation_allowed(0x20, n, 0x700000,
							    0, 0));
		}
	assert(!apple_qspi_mutation_allowed(2, 3, UINT64_MAX, 1, 1));
	assert(!apple_qspi_mutation_allowed(2, 3, 0x100700000ULL, 1, 1));
	assert(!apple_qspi_mutation_allowed(2, 3, 0x700000, UINT32_MAX, 1));
	for (s1 = 0; s1 < 256; s1++)
		for (s2 = 0; s2 < 256; s2++)
			for (s3 = 0; s3 < 256; s3++) {
				int accepted = apple_qspi_status_allows_write(
					s1, s2, s3);
				assert(accepted == (!(s1 & 0x1f) &&
						    !(s2 & 0xc0) && !(s3 & 4)));
			}
	puts("PASS: 24-bit addresses, page boundaries, opcodes, overflow, all status combinations");
}
