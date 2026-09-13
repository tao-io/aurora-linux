#!/usr/bin/env python3
"""Compare layered G16 Rust register state with the native-derived C producer.

Offline only: --kernel points to linux-asahi; --output retains binaries and
results. The reference is a source transcription, not a hardware capture.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
source = a.kernel / 'drivers/gpu/drm/asahi/tools'
native, rust = a.output / 'native', a.output / 'rust'
subprocess.run(['cc', '-O2', str(source / 'm4_render_state_reference.c'), '-o', str(native)], check=True)
subprocess.run(['rustc', '--edition=2021', str(source / 'm4_render_state_fixture.rs'), '-o', str(rust)], check=True)
checks = 0
for layers in (1, 2, 4, 2048):
    for empty in (0, 1):
        outputs = []
        for binary in (native, rust):
            data = subprocess.check_output([str(binary.resolve()), str(layers), str(empty)])
            (a.output / f'{binary.name}-{layers}-{empty}.bin').write_bytes(data)
            assert len(data) == 6 * 0xca0
            outputs.append(data)
        for i in range(6):
            start = 0x80 if i % 2 else 0x40
            def registers(data):
                result = {}
                for off in range(i * 0xca0 + start, i * 0xca0 + start + 0x600, 12):
                    key, value = struct.unpack_from('<IQ', data, off)
                    if key:
                        result[key & ~1] = value
                return result
            reference, actual = map(registers, outputs)
            keys = ((0x16060, 0x16098, 0x10038, 0x16068) if i % 2 else
                    (0x10168, 0x1c078, 0x1c9d8, 0x1c030, 0x1c9c0))
            for key in keys:
                assert reference[key] == actual[key], (layers, empty, i, hex(key), reference[key], actual[key])
                checks += 1
            if i % 2:
                for data in outputs:
                    assert struct.unpack_from('<I', data, i * 0xca0 + 0x50)[0] == 1
                    checks += 1
result = dict(passed=True, checks=checks, qualification='offline native-derived encoder comparison')
(a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print('PASS:', checks, 'layer register and mirror comparisons')
