/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_RENDER_STREAM_H
#define G16_RENDER_STREAM_H

/* M4 direct, non-indexed triangle stream. PPP state packets are still the
 * identified reference records; only their explicit addresses enter here.
 */
#define G16_RENDER_STREAM_BYTES 120
static inline int g16_render_pack_stream(u8 *out, size_t bytes,
					u64 vertex_loader, u32 count, u32 first)
{
	static const struct { u32 address, words; } ppp[] = {
		{0x40, 5}, {0x58000, 7}, {0x5801c, 5}, {0x58030, 7},
		{0x5804c, 5}, {0x68900, 10}, {0x58060, 3},
		{0x5806c, 2}, {0x48000, 2},
	};
	u32 offset = 0, i;

	if (!out || bytes < G16_RENDER_STREAM_BYTES || !vertex_loader ||
	    vertex_loader >= (1ULL << 42) || (vertex_loader & 63) ||
	    !count || first >= 6 || count > 6 - first)
		return -EINVAL;
#define WORD(v) do { g16_render_put(out + offset, (v), 4); offset += 4; } while (0)
	/* VDM state: VS words 0/1, four position outputs and unknown state.
	 * M4 uses a separate high address word before the 64-byte-unit PC.
	 */
	WORD(0x40000000 | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5));
	WORD(0); /* fixed qualified resource allocation */
	WORD((vertex_loader >> 32) << 16);
	WORD((vertex_loader & 0xffffffff) >> 6);
	WORD(4 | (4 << 8));
	WORD(0);
	WORD(0);
	for (i = 0; i < sizeof(ppp) / sizeof(ppp[0]); i++) {
		WORD(ppp[i].words << 8);
		WORD(ppp[i].address);
	}
	/* Triangle list, native U32 index-format bits (no index buffer),
	 * explicit vertex count, instance count and first vertex.
	 */
	WORD(0x60000000 | (6 << 8) | (2 << 17) | (1 << 22) | (1 << 23) | (1 << 24));
	WORD(count);
	WORD(1);
	WORD(first);
	WORD(0xc0000000); /* terminate */
#undef WORD
	return offset;
}
#endif
