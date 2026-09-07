#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bounded RGBA8 layout decoding and PNG packaging; does not render pixels."""
import struct
import zlib


def detile_rgba8(raw, size):
    if size not in (4, 32, 256) or len(raw) != size * size * 4:
        raise ValueError('unexpected square RGBA8 target')
    tile = min(size, 64)
    result = bytearray(len(raw))
    for y in range(size):
        for x in range(size):
            index = ((y // tile) * (size // tile) + x // tile) * tile * tile
            for bit in range(tile.bit_length() - 1):
                index |= ((x >> bit) & 1) << (bit * 2)
                index |= ((y >> bit) & 1) << (bit * 2 + 1)
            result[(y * size + x) * 4:(y * size + x + 1) * 4] = raw[index * 4:index * 4 + 4]
    return bytes(result)


def write_png(path, rgba, size):
    if len(rgba) != size * size * 4:
        raise ValueError('wrong RGBA8 byte count')

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    image = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
    image += chunk(b'IDAT', zlib.compress(b''.join(b'\0' + rgba[y * size * 4:(y + 1) * size * 4]
                                                 for y in range(size))))
    path.write_bytes(image + chunk(b'IEND', b''))


def linear_varying_reference(flip=False, uniform=False):
    """Pixel-center linear interpolation, fixed clip-space right triangle.

    Red attributes are (0,1,0), or (1,0,1) on alternate frames. This is
    comparison data only and must never be uploaded as render-target content.
    """
    def red(x):
        if uniform:
            return 0 if flip else 255
        return (((511 - 2*x) if flip else (2*x + 1)) * 255 + 256) // 512

    return bytes(channel for y in range(256) for x in range(256)
                 for channel in ((red(x),
                                   0, 0, 255) if x < y else (0, 0, 0, 0)))


def triangle_reference(expanded=False):
    """CPU comparison only; never copied into the GPU target."""
    return bytes(v for y in range(256) for x in range(256)
                 for v in (((max(y - x, 0) * 255 + 128) // 256,
                            (x * 255 + 128) // 256,
                            ((256 - y) * 255 + 128) // 256, 255)
                           if x < y or expanded else (0, 0, 0, 0)))
