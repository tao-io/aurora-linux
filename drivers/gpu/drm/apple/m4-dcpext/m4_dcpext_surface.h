/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCPEXT_SURFACE_H
#define M4_DCPEXT_SURFACE_H
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define M4_DCPEXT_SURFACE_SIZE 0x22c

/* M4 linear BGRA wire record, shared with the qualified internal KMS layout.
 * Native RGB colorspace/SDR transfer follow apple/plane.c. The external
 * owner must separately validate the selected mode, backing size and DVA. */
static inline int m4_dcpext_surface_build(void *output, u32 width, u32 height,
					u32 stride, bool opaque)
{
	u8 *s = output;
	u32 bytes;

	if (!s || !width || width > 1920 || !height || height > 1080 ||
	    stride < width * 4 || stride > 7680 || (stride & 63))
		return -EINVAL;
	bytes = stride * height;
	memset(s, 0, M4_DCPEXT_SURFACE_SIZE);
	s[2] = opaque;
	put_unaligned_le32(1, s + 3);
	put_unaligned_le32(1, s + 7);
	put_unaligned_le32(0x42475241, s + 0xb);
	s[0x13] = 13;
	s[0x14] = 12;
	put_unaligned_le32(stride, s + 0x15);
	put_unaligned_le16(1, s + 0x19);
	s[0x1b] = s[0x1c] = 1;
	put_unaligned_le32(width, s + 0x21);
	put_unaligned_le32(height, s + 0x25);
	put_unaligned_le32(bytes, s + 0x29);
	put_unaligned_le32(1, s + 0x35);
	put_unaligned_le64(1, s + 0x51);
	put_unaligned_le32(width, s + 0x59);
	put_unaligned_le32(height, s + 0x5d);
	put_unaligned_le32(stride, s + 0x69);
	put_unaligned_le32(bytes, s + 0x6d);
	put_unaligned_le16(4, s + 0x71);
	s[0x73] = s[0x74] = 1;
	put_unaligned_le64(1, s + 0x149);
	return 0;
}
#endif
