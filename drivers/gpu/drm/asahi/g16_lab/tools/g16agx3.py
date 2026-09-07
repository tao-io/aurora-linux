#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build/use Aurora's pinned AGX3b encoder for offline M4 shader generation.

The shared library is a host tool only. No Apple binary or captured shader is
linked. Build with --mesa PATH --output PATH, then pass that library to the
graphics generator. The kernel build uses the checked-in generated headers.
"""
import argparse
import ctypes as C
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

MESA_REVISION = 'e58d46899eb1f351863bccab4b6e8c87ed32b95e'
SOURCES = ['agx3_vary_core.c', 'agx3_tex_core.c', 'agx3_binder.c',
           'agx3_compose/agx3_compose.c', 'agx3_compose/agx3b_tables.c',
           'agx3_compose/agx3c_tables.c']


def build(mesa, output):
    mesa, output = mesa.resolve(), output.resolve()
    revision = subprocess.check_output(
        ['git', '-C', str(mesa), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != MESA_REVISION:
        raise ValueError(f'expected Mesa {MESA_REVISION}, got {revision}')
    dirty = subprocess.check_output(
        ['git', '-C', str(mesa), 'status', '--porcelain', '--',
         'src/asahi/compiler', 'src/asahi/lib/agx_chip.h', 'include/drm-uapi'],
        text=True)
    if dirty:
        raise ValueError('pinned encoder source has local changes')
    compiler = mesa / 'src/asahi/compiler'
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='g16-agx3-') as temp:
        shim = Path(temp) / 'identity.c'
        shim.write_text('#include "asahi/lib/agx_chip.h"\n'
                        'int g16_agx3_encoding(void) { return AGX_ISA_ENCODING_AGX3B; }\n')
        artifact = Path(temp) / 'encoder.so'
        command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-shared', '-fPIC', '-Wl,-z,defs',
                   '-I' + str(mesa / 'include'), '-I' + str(mesa / 'src'),
                   '-I' + str(compiler), str(shim),
                   *[str(compiler / source) for source in SOURCES],
                   '-o', str(artifact)]
        subprocess.run(command, check=True)
        output.write_bytes(artifact.read_bytes())
    manifest = {'mesa_revision': revision,
                'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'sources': SOURCES, 'scope': 'host-only AGX3b encoder'}
    output.with_suffix(output.suffix + '.json').write_text(
        json.dumps(manifest, indent=2) + '\n')


class Encoder:
    def __init__(self, library):
        self.lib = C.CDLL(str(Path(library).resolve()))
        self.lib.g16_agx3_encoding.restype = C.c_int
        self.encoding = self.lib.g16_agx3_encoding()
        byteptr = C.POINTER(C.c_uint8)
        signatures = {
            'agx3_vary_uvsw': [C.c_int, C.c_uint16, C.c_uint16, C.c_uint8,
                              C.c_bool, C.c_uint16],
            'agx3_tex_stage_copy': [C.c_int, C.c_uint16, C.c_uint16,
                                   C.c_uint8, C.c_uint16],
            'agx3_vary_interp_at_start': [C.c_int, C.c_uint16, C.c_uint16,
                                         C.c_bool, C.c_int, C.c_bool],
        }
        for name, signature in signatures.items():
            fn = getattr(self.lib, name)
            fn.argtypes = signature + [byteptr, byteptr]
            fn.restype = C.c_bool

    def emit(self, name, *args):
        # Keep Python integers from silently wrapping at the C ABI boundary.
        fn = getattr(self.lib, name)
        for value, kind in zip(args, fn.argtypes[1:-2]):
            maximum = 1 if kind == C.c_bool else (1 << (8 * C.sizeof(kind))) - 1
            if not isinstance(value, int) or not 0 <= value <= maximum:
                raise ValueError(f'{name}: argument out of range: {value}')
        out, length = (C.c_uint8 * 32)(), C.c_uint8()
        if not fn(self.encoding, *args, out, C.byref(length)) or not 0 < length.value <= 32:
            raise ValueError(f'{name}: unsupported operands {args}')
        return bytes(out[:length.value])

    def export(self, register, index, slot, wait):
        return self.emit('agx3_vary_uvsw', register, index, slot, True, wait)

    def stage(self, dest, source, slot, wait=0):
        return self.emit('agx3_tex_stage_copy', dest, source, slot, wait)

    def interpolate(self, dest, coefficient, first=False):
        return self.emit('agx3_vary_interp_at_start', dest, coefficient,
                         False, 0, first)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build(args.mesa, args.output)
