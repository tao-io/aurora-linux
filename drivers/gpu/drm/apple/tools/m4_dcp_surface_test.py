#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check a native M4 surface through its hash-pinned firmware constructor offline."""
import argparse
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r
from m4_dcp_dispatch import checked_image, TEXT_SHA, DATA_SHA
sys.path.insert(0, str(Path(__file__).parent/'rtkit-probe'))
from surface import scanout_surface

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('text', type=Path); p.add_argument('data', type=Path)
a=p.parse_args()
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
for base,blob in ((0,checked_image(a.text,TEXT_SHA)),(0x806000,checked_image(a.data,DATA_SHA))):
    u.mem_map(base,(len(blob)+4095)&~4095);u.mem_write(base,blob)
u.mem_map(0x1000000,0x100000)
obj,wire,stop=0x1000000,0x1001000,0x1002000
info=dict(width=2560,height=1664,stride=10240,size=17039360)
surf=scanout_surface(info)
u.mem_write(wire,bytes(surf))
u.reg_write(r.UC_ARM64_REG_X0,obj);u.reg_write(r.UC_ARM64_REG_X1,wire)
u.reg_write(r.UC_ARM64_REG_X30,stop);u.reg_write(r.UC_ARM64_REG_SP,0x10f0000)
def hook(uc,address,size,unused):
    word=int.from_bytes(uc.mem_read(address,4),'little')
    if address in (0x4d3c,0x4efc):
        dest=uc.reg_read(r.UC_ARM64_REG_X0);src=uc.reg_read(r.UC_ARM64_REG_X1);count=uc.reg_read(r.UC_ARM64_REG_X2)
        blob=bytes([src&255])*count if address==0x4d3c else bytes(uc.mem_read(src,count))
        uc.mem_write(dest,blob);uc.reg_write(r.UC_ARM64_REG_PC,uc.reg_read(r.UC_ARM64_REG_X30))
    elif word==0xd503237f or word&0xfffffc00==0xdac10800:
        uc.reg_write(r.UC_ARM64_REG_PC,address+4) # synthetic PAC only
    elif word==0xd65f0fff:
        uc.reg_write(r.UC_ARM64_REG_PC,uc.reg_read(r.UC_ARM64_REG_X30))
u.hook_add(UC_HOOK_CODE,hook)
u.emu_start(0x1cb078,stop,count=200)
assert u.reg_read(r.UC_ARM64_REG_PC)==stop
actual=bytes(u.mem_read(obj+16,0x22c));expected=bytearray(surf)
for off,value in ((0x51,obj+0x49),(0x149,obj+0x69),(0x1ed,0)):
    struct.pack_into('<Q',expected,off,value)
assert actual==expected
assert struct.unpack_from('<III',actual,0x21)==(2560,1664,17039360)
for bad in (dict(info,width=0),dict(info,size=1),dict(info,stride=1)):
    try: scanout_surface(bad)
    except ValueError: pass
    else: raise AssertionError('invalid extent accepted')
print('PASS: M4 surface constructor preserves fields and relocates presence pointers; invalid extents rejected')
