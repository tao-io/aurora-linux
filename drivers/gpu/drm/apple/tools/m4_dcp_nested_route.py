#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline check of M4 DCP interrupt routing with an occupied parent stream."""
import argparse
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r
from m4_dcp_dispatch import checked_image, TEXT_SHA


def route(text, message):
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    u.mem_map(0,(len(text)+4095)&~4095);u.mem_write(0,text)
    base=0x1000000;u.mem_map(base,0x10000)
    def put(off,fmt,value):u.mem_write(base+off,struct.pack(fmt,value))
    put(0x70,'<I',1)  # Firmware owns side 1.
    put(0x278,'<I',1)  # Remote stream zero already owns the A401 request.
    put(0x58,'<Q',base+0x2000)
    put(0x2000,'<B',1)  # Main RX condition initialized.
    u.reg_write(r.UC_ARM64_REG_X0,base)
    u.reg_write(r.UC_ARM64_REG_X1,message)
    u.reg_write(r.UC_ARM64_REG_SP,base+0xf000)
    result={}
    stops={0x197f6c:'owner-rejected',0x197f7c:'stream-not-free',0x197f34:'condition-invalid'}
    def hook(uc,address,size,unused):
        if address in stops:
            result['route']=stops[address];uc.emu_stop()
        elif address==0x197f30:
            result['route']='main-handler' if uc.reg_read(r.UC_ARM64_REG_W0)==0 else 'irq-worker'
            uc.emu_stop()
        elif address==0x197e74:
            uc.reg_write(r.UC_ARM64_REG_PC,address+4)  # PACIBSP, synthetic stack.
    u.hook_add(UC_HOOK_CODE,hook)
    u.emu_start(0x197e74,0x197f8c,count=100)
    if 'route' not in result:raise RuntimeError('unexpected IRQ-handler exit')
    return {'message':hex(message),**result}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('text',type=Path);a=p.parse_args()
    text=checked_image(a.text,TEXT_SHA)
    cases=[(0x1000400202,'stream-not-free'),(0x1000000302,'owner-rejected'),
           (0x1000400002,'main-handler')]
    results=[]
    for message,expected in cases:
        result=route(text,message)
        if result['route']!=expected:raise RuntimeError(result)
        results.append(result)
    print(json.dumps({'text_sha256':TEXT_SHA,'checks':results},indent=2))

if __name__=='__main__':main()
