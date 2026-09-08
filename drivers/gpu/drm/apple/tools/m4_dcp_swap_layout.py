#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check J713 A407 marshalling offline; never execute its display implementation."""
import argparse
import json
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r
from m4_dcp_dispatch import checked_image, TEXT_SHA, DATA_SHA


def check(text, data, surfaces):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for base, blob in ((0, text), (0x806000, data)):
        u.mem_map(base, (len(blob)+4095)&~4095)
        u.mem_write(base, blob)
    base = 0x1000000
    u.mem_map(base, 0x100000)
    ctx, channel, packet, output, obj, vtable = [base+x for x in (0, 0x1000, 0x2000, 0x4000, 0x5000, 0x6000)]
    def put(address, fmt, *values):
        u.mem_write(address, struct.pack(fmt, *values))
    def getreg(n):
        return u.reg_read(getattr(r, 'UC_ARM64_REG_X'+str(n)))
    def setreg(n, value):
        u.reg_write(getattr(r, 'UC_ARM64_REG_X'+str(n)), value)
    put(0xae9358, '<Q', obj+16)
    put(obj, '<Q', vtable)
    wire = bytearray(0x1bd8)
    wire[0x1bcb:0x1bd5] = bytes([0 if surfaces else 1])*10
    put(packet, f'<{len(wire)}s', wire)
    for n, value in enumerate((ctx, channel, packet, len(wire), output, 12)):
        setreg(n, value)
    u.reg_write(r.UC_ARM64_REG_SP, base+0xf0000)
    allocation = base+0x10000
    constructors = []
    forwarded = {}
    def hook(uc, address, size, unused):
        nonlocal allocation
        word = int.from_bytes(uc.mem_read(address, 4), 'little')
        if address == 0x1e2bd0:
            uc.emu_stop()
        elif address in (0x1e2c50, 0x1e2c78, 0x1e2cec, 0x1e2d14):
            length = getreg(0)
            setreg(0, allocation)
            allocation += (length+63)&~63
            uc.reg_write(r.UC_ARM64_REG_PC, address+4)
        elif address in (0x1e2c88, 0x1e2d24):
            constructors.append(getreg(1)-packet)
            put(getreg(0), '<Q', vtable)
            uc.reg_write(r.UC_ARM64_REG_PC, address+4)
        elif address in (0x1e2ca4, 0x1e2d40):
            setreg(0, 1)
            uc.reg_write(r.UC_ARM64_REG_PC, address+4)
        elif address == 0x1e2e44:
            # Stop the virtual display call: only inspect the marshalled arguments.
            forwarded.update(swap=getreg(1)-packet, primary_iovas=getreg(3)-packet,
                             primary_extra=getreg(4)-packet, secondary_iovas=getreg(6)-packet,
                             primary_pointers=list(struct.unpack('<4Q', uc.mem_read(getreg(2), 32))),
                             secondary_pointers=list(struct.unpack('<6Q', uc.mem_read(getreg(5), 48))))
            setreg(0, 0)
            uc.reg_write(r.UC_ARM64_REG_PC, address+4)
        elif address in (0x1e2e48, 0x1e2e78, 0x1e2e8c, 0x1e2eb4, 0x1e2ec8):
            setreg(0, 0)
            uc.reg_write(r.UC_ARM64_REG_PC, address+4)
        elif word == 0xd503237f or word & 0xfffffc00 == 0xdac11800:
            # PACIBSP / AUTDA only authenticate synthetic pointers here.
            uc.reg_write(r.UC_ARM64_REG_PC, address+4)
    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(0x1e2b80, 0x1e2f10, count=2000)
    if u.reg_read(r.UC_ARM64_REG_PC) != 0x1e2bd0 or getreg(0) != 0:
        raise RuntimeError('unexpected wrapper exit')
    expected = [0x588+i*0x22c for i in range(4)] + [0xe78+i*0x22c for i in range(6)]
    assert constructors == (expected if surfaces else [])
    assert {k:forwarded[k] for k in ('swap','primary_iovas','primary_extra','secondary_iovas')} == {
        'swap':0, 'primary_iovas':0xe38, 'primary_extra':0xe58, 'secondary_iovas':0x1b80}
    assert all(bool(p) == surfaces for k in ('primary_pointers','secondary_pointers') for p in forwarded[k])
    return {'surfaces_present':surfaces, 'surface_offsets':constructors,
            'wire_size':len(wire), 'forwarded_offsets':{k:v for k,v in forwarded.items() if not k.endswith('pointers')}}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('text', type=Path); p.add_argument('data', type=Path)
    a=p.parse_args()
    text=checked_image(a.text,TEXT_SHA); data=checked_image(a.data,DATA_SHA)
    print(json.dumps({'text_sha256':TEXT_SHA, 'data_sha256':DATA_SHA,
                      'checks':[check(text,data,False),check(text,data,True)]},indent=2))

if __name__ == '__main__':
    main()
