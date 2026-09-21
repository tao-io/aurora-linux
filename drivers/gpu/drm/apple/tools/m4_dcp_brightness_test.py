#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the production encoder against the qualified M4 firmware marshaller.

Offline only. Use build/display-venv/bin/python and the retained September 8
__TEXT snapshot. This does not prove that the physical backlight is illuminated.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as r

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('text', type=Path)
a = p.parse_args()
text = a.text.read_bytes()
digest = hashlib.sha256(text).hexdigest()
assert digest == '25b095cc293adb48c2c507a578b94810e5e06e7607eb13e5d64a6221e2c29793'
header = Path(__file__).parent / 'rtkit-probe/m4_dcp_brightness.h'
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'linux').mkdir()
    (tmp / 'linux/bitops.h').write_text('''
#include <stdint.h>
typedef uint8_t u8;
typedef uint64_t u64;
#define fls(x) ((x) ? 32 - __builtin_clz(x) : 0)
''')
    (tmp / 'linux/unaligned.h').write_text('''
#include <string.h>
static inline void put_unaligned_le64(uint64_t v, void *p) { memcpy(p, &v, 8); }
''')
    (tmp / 'test.c').write_text('''
#include <assert.h>
#include <stdio.h>
#include "m4_dcp_brightness.h"
int main(void) {
    for (unsigned int n = 0; n <= M4_PANEL_MAX_NITS; n++) {
        u8 wire[0x1bd8] = {0}, expected[0x1bd8] = {0};
        double value = n;
        m4_dcp_encode_brightness(wire, n);
        expected[0x354] = 1;
        memcpy(expected + 0x35e, &value, 8);
        assert(!memcmp(wire, expected, sizeof(wire)));
        if (n == 0 || n == 1 || n == 120 || n == 500)
            assert(fwrite(wire, sizeof(wire), 1, stdout) == 1);
    }
}
''')
    subprocess.run(['cc', '-Wall', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(tmp), '-I', str(header.parent), str(tmp / 'test.c'),
                    '-o', str(tmp / 'test')], check=True)
    wires = subprocess.check_output([str(tmp / 'test')])

checks = []
for index, nits in enumerate((0, 1, 120, 500)):
    wire = wires[index * 0x1bd8:(index + 1) * 0x1bd8]
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0, (len(text) + 4095) & ~4095)
    u.mem_write(0, text)
    base = 0x1000000
    u.mem_map(base, 0x10000)
    transaction = base + 0x2000
    pipeline = base + 0x4000
    properties = base + 0x6000
    u.mem_write(base, wire)
    for n, value in ((20, base), (22, transaction),
                     (23, transaction + 0x364), (28, base + 0x154)):
        u.reg_write(getattr(r, f'UC_ARM64_REG_X{n}'), value)
    u.emu_start(0x1d554c, 0x1d55a8, count=100)
    assert u.reg_read(r.UC_ARM64_REG_PC) == 0x1d55a8
    assert bytes(u.mem_read(transaction + 0x799, 8)) == b'\1' + bytes(7)
    assert struct.unpack('<d', u.mem_read(transaction + 0x7a8, 8))[0] == nits
    # Stop before the runtime-property call; check its actual firmware arguments.
    u.mem_write(pipeline + 0x1cc8, struct.pack('<Q', properties))
    u.mem_write(properties, struct.pack('<Q', base + 0x8000))
    for n, value in ((19, transaction), (20, pipeline)):
        u.reg_write(getattr(r, f'UC_ARM64_REG_X{n}'), value)
    u.emu_start(0x2413a8, 0x2413c4, count=20)
    assert u.reg_read(r.UC_ARM64_REG_PC) == 0x2413c4
    assert u.reg_read(r.UC_ARM64_REG_X1) == 0x13
    assert u.reg_read(r.UC_ARM64_REG_X2) == nits << 16
    checks.append(dict(nits=nits, property_id=19, value_16_16=nits << 16))
print(json.dumps(dict(text_sha256=digest, header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                      encoder_cases=501, firmware_marshalling_checks=checks,
                      scope='Offline encoding/firmware ABI; no hardware claim'), indent=2))
