#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline comparison with an owned M4/25G83 kext; never accesses hardware.

Requires Unicorn 2.1.4, a host C compiler and llvm-objdump. Native images are
not distributed. Only local in-memory copies are adapted for emulation:
pointer-auth instructions become no-ops, authenticated calls become ordinary
calls, and platform-default virtual methods/global debug overrides are stubbed.
Register encoders and geometry arithmetic execute from the original ARM64 code.
This tests translation fidelity, not shader execution or valid draw inputs.
"""
import argparse, hashlib, struct, re, random, ctypes as C
import subprocess, tempfile
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('kext',type=Path,help='25G83 extracted com.apple.AGXG16G')
args=parser.parse_args()
work=tempfile.TemporaryDirectory(prefix='g16-render-native-')
root=Path(__file__).resolve().parent.parent
so=Path(work.name)/'oracle.so'
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
               '-I'+str(root/'render'),str(root/'render/test_native.c'),'-o',str(so)],check=True)
disassembly=subprocess.check_output(['llvm-objdump','--disassemble','--no-show-raw-insn',str(args.kext)],text=True)

from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
P=args.kext
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
b=P.read_bytes();off=32
digest=hashlib.sha256(b).hexdigest()
print('native kext SHA256:',digest)
if digest != '5a62d601d5b32301efcd89b144c08874c601d80b40f4b2fd64819537e07a31cf':
 raise SystemExit('unrecognized native image: this oracle is specific to the recorded 25G83 kext')
if b[:4] != bytes.fromhex('cffaedfe'):
 raise SystemExit('expected a little-endian 64-bit Mach-O image')
for _ in range(struct.unpack_from('<I',b,16)[0]):
 cmd,size=struct.unpack_from('<II',b,off)
 if cmd==0x19:
  va,vs,fo,fs=struct.unpack_from('<QQQQ',b,off+24)
  start=va&~4095;end=(va+vs+4095)&~4095
  u.mem_map(start,end-start);u.mem_write(va,b[fo:fo+fs])
 off+=size
for line in disassembly.splitlines():
 m=re.match(r'([0-9a-f]+):\s+(\w+)\s*(.*)',line)
 if not m:continue
 a=int(m[1],16);op=m[2];args=m[3]
 if op.startswith(('pac','aut','xpac')):u.mem_write(a,struct.pack('<I',0xd503201f))
 elif op=='retab':u.mem_write(a,struct.pack('<I',0xd65f03c0))
 elif op in ('blraa','blrab','braa','brab'):
  r=int(re.match(r'x(\d+)',args)[1]);u.mem_write(a,struct.pack('<I',(0xd63f0000 if op.startswith('bl') else 0xd61f0000)|(r<<5)))
BASE=0x200000;ACC=BASE;CHANNEL=BASE+0x10000;DESC=BASE+0x11000;CMD=BASE+0x13000;PB=BASE+0x15000;DBG=BASE+0x16000;STACK=BASE+0x3f000;STOP=BASE+0x17000;VT=BASE+0x18000
u.mem_map(BASE,0x40000)
def put(a,v,n=8):u.mem_write(a,v.to_bytes(n,'little'))
def get(a,n=8):return int.from_bytes(u.mem_read(a,n),'little')
put(0xfffffe0007e9db90,0xfffffe00089c9fa0)
put(ACC,VT);put(CHANNEL+0x10,ACC);put(ACC+0xf540,DBG)
put(VT+0x1098,STOP+8);put(VT+0x10a0,STOP+12)
# No global debug overrides; stub platform virtual defaults only.
def hook(u,a,size,_):
 if a==STOP:u.emu_stop()
 elif a==STOP+16:
  u.reg_write(UC_ARM64_REG_X0,CMD);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_X30))
 elif a in (STOP+8,STOP+12,0xfffffe00089c9e30):
  if a!=0xfffffe00089c9e30:u.reg_write(UC_ARM64_REG_X0,0x11223344 if a==STOP+8 else 0x55667788)
  u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_X30))
u.hook_add(UC_HOOK_CODE,hook)
lib=C.CDLL(str(so))
class TP(C.Structure):
 _fields_=[('flags',C.c_uint64),('slot0',C.c_uint64),('slot28',C.c_uint64),('default',C.c_uint64),('count',C.c_uint32),('flag',C.c_uint8),('disable',C.c_uint8),('debug',C.c_uint8)]
class FP(C.Structure):
 _fields_=[('flags',C.c_uint64),('slot28',C.c_uint64),('default',C.c_uint64),('count',C.c_uint32),('flag',C.c_uint8),('ed20',C.c_uint8),('disable',C.c_uint8),('debug',C.c_uint8)]
rng=random.Random(713)
for stage,fn,regoff,modeoff in [('ta',0xfffffe00089cbcb0,0x40,0x830),('fragment',0xfffffe00089c162c,0x80,0x9d8)]:
 for case in range(100):
  d=bytearray(rng.randbytes(0x1428)) if case else bytearray(0x1428)
  struct.pack_into('<I',d,0x1038,rng.choice([1,2,8]))
  for x in (0x890,0x891,0x1410):d[x]=rng.choice([0,1,2,3,4])
  struct.pack_into('<Q',d,0x848,PB);struct.pack_into('<Q',d,0x10e8,PB)
  struct.pack_into('<Q',d,0x408,0x100000);struct.pack_into('<Q',d,0xe98,0x100000)
  for x in (0x41c,0x420):struct.pack_into('<I',d,x,rng.choice([16,32]))
  struct.pack_into('<I',d,0x7f8,rng.choice([1,2,4]))
  if case%3==0:
   struct.pack_into('<Q',d,0x1378,0);struct.pack_into('<Q',d,0xb58,0)
  flags=rng.getrandbits(64);count=rng.choice([1,4,10]);flag=rng.randrange(2);debug=rng.randrange(2);disable=rng.randrange(2);ed20=rng.randrange(2)
  slot0=rng.getrandbits(64);slot28=rng.getrandbits(64)
  put(ACC+0x6a8,flags);put(ACC+0x4e4,count,4);put(ACC+0x6b1,flag,1);put(ACC+0xed20,ed20,1);put(ACC+0xf940,disable,1);put(DBG+0x54,debug,1)
  put(PB,slot0);put(PB+0x28,slot28)
  u.mem_write(DESC,bytes(d));u.mem_write(CMD,bytes(0x1000));put(CMD+(0x898 if stage=='ta' else 0xbe0),0x12345678,4)
  for r,v in [(UC_ARM64_REG_X0,CHANNEL),(UC_ARM64_REG_X1,CMD),(UC_ARM64_REG_X2,DESC),(UC_ARM64_REG_X30,STOP),(UC_ARM64_REG_SP,STACK)]:u.reg_write(r,v)
  try:u.emu_start(fn,STOP,count=50000)
  except Exception:
   print(stage,case,'PC',hex(u.reg_read(UC_ARM64_REG_PC)));raise
  if u.reg_read(UC_ARM64_REG_PC)!=STOP:raise RuntimeError('instruction budget exhausted')
  p=TP(flags,slot0,slot28,0x5566778811223344,count,flag,disable,debug) if stage=='ta' else FP(flags,slot28,0x5566778811223344,count,flag,ed20,disable,debug)
  out=C.create_string_buffer(1536);mode=C.c_uint64()
  n=getattr(lib,stage)(out,bytes(d),C.byref(p),C.byref(mode))
  count_native=get(CMD+regoff+0x708,2);native=bytes(u.mem_read(CMD+regoff,count_native*12))
  if n!=count_native or out.raw[:n*12]!=native or mode.value!=get(CMD+modeoff):
   print(stage,case,'counts',n,count_native,'mode',hex(mode.value),hex(get(CMD+modeoff)))
   for i in range(max(n,count_native)):
    x=out.raw[i*12:i*12+12];y=native[i*12:i*12+12]
    if x!=y:print(i,'C',x.hex(),'native',y.hex())
   raise SystemExit(1)
 print(stage,'100/100 byte-identical register arrays and mode words')
class Geometry(C.Structure):
 _fields_=[(n,C.c_uint32) for n in ('pixels','screen','x_blocks','y_blocks','blocks','tiles','region_stride','tpc_stride','isp_blocks')]+[(n,C.c_uint64) for n in ('utile_config','tilemap_bytes','tpc_bytes')]
for case in range(100):
 w,h=(4,4) if not case else (rng.randrange(1,16385),rng.randrange(1,16385))
 x,y=rng.choice((16,32)),rng.choice((16,32));count=rng.choice((1,4,10))
 u.mem_write(DESC,bytes(0x400));put(DESC+0x120,ACC);put(ACC+0x4e4,count,4)
 for o,v in ((0x40,w),(0x44,h),(0x48,1),(0x90,x),(0x94,y),(0x98,1),(0x3c,1)):put(DESC+o,v,4)
 for r,v in [(UC_ARM64_REG_X0,DESC),(UC_ARM64_REG_X1,0),(UC_ARM64_REG_X2,128),(UC_ARM64_REG_X30,STOP),(UC_ARM64_REG_SP,STACK)]:u.reg_write(r,v)
 u.emu_start(0xfffffe00089ca2a0,STOP,count=10000)
 g=Geometry();assert lib.geometry(C.byref(g),w,h,x,y,count)==0
 for name,o in [('pixels',0x100),('screen',0x104),('x_blocks',0x108),('y_blocks',0x10c),('blocks',0x110),('tiles',0x118),('region_stride',0x18),('tpc_stride',0x28),('isp_blocks',0xbc)]:
  assert getattr(g,name)==get(DESC+o,4),(case,name,hex(getattr(g,name)),hex(get(DESC+o,4)))
 assert g.utile_config==get(DESC+0xf0)
 assert g.tpc_bytes==get(DESC+0x140)
 assert g.tilemap_bytes==(get(DESC+0x10)+4095)&~4095
print('geometry 100/100 matches native register values and allocation sizes')

put(ACC+0xf8d0,PB);put(PB+0x60,DBG);put(DBG,VT);put(VT+0x138,STOP+16)
lib.pb_hw.argtypes=[C.c_void_p,C.c_uint64,C.c_uint32,C.c_uint32,C.c_uint32,C.c_uint8,C.c_uint8]
for case in range(100):
 va=(rng.getrandbits(43)//128)*128;pages=rng.randrange(1,0x400000)
 read=rng.randrange(0x400000);write=rng.randrange(0x400000)
 wrap=rng.randrange(2);valid=rng.randrange(2);original=rng.randbytes(16)
 u.mem_write(CMD,original);put(DESC+0x28,va);put(DESC+0x34,pages,4)
 u.mem_write(DESC+0x100,struct.pack('<IIII',read,write,wrap,valid))
 for r,v in [(UC_ARM64_REG_X0,ACC),(UC_ARM64_REG_X1,DESC),(UC_ARM64_REG_X2,DESC+0x100),(UC_ARM64_REG_X3,0),(UC_ARM64_REG_X30,STOP),(UC_ARM64_REG_SP,STACK)]:u.reg_write(r,v)
 u.emu_start(0xfffffe00089c4338,STOP,count=10000)
 out=C.create_string_buffer(original,16)
 assert lib.pb_hw(out,va,pages,read,write,wrap,valid)==0
 assert out.raw==bytes(u.mem_read(CMD,16)),(case,out.raw.hex(),bytes(u.mem_read(CMD,16)).hex())
print('PB hardware descriptor 100/100 byte-identical, including preserved bits')
