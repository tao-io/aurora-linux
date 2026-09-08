# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""M4 linear BGRA diagnostic surface, using the qualified 0x22c wire size."""
import struct


def scanout_surface(info):
    width, height, stride = (info[k] for k in ('width', 'height', 'stride'))
    if not 0 < width <= 2560 or not 0 < height <= 1664 or stride < width*4 or info['size'] < stride*height:
        raise ValueError('invalid scanout extent')
    surface = bytearray(0x22c)
    # M4 0x1cb078 copies this record after the 16-byte object header and
    # replaces nonzero presence flags at 0x51/0x149/0x1ed with local pointers.
    struct.pack_into('<III', surface, 3, 1, 1, 0x42475241)
    surface[0x13:0x15] = bytes((13, 12))  # SDR transfer, native panel colorspace
    struct.pack_into('<IHBB', surface, 0x15, stride, 1, 1, 1)
    struct.pack_into('<III', surface, 0x21, width, height, stride*height)
    struct.pack_into('<I', surface, 0x35, 1)
    struct.pack_into('<Q', surface, 0x51, 1)
    struct.pack_into('<IIIIIIHBB', surface, 0x59, width, height, 0, 0, stride, stride*height, 4, 1, 1)
    struct.pack_into('<Q', surface, 0x149, 1)
    return surface
