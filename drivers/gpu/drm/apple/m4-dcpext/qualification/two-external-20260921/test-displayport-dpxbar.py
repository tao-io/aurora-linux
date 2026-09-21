#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compare actual DPXBAR C with the native offline model; inject cleanup faults."""
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
reference = json.loads((root / 'build/displayport-kernelcache/atc-dpxbar-native-model-source2.json').read_text())
assert reference['source'] == '0x8010' and reference['destination'] == '0x8010'
golden = []
for event in reference['events']:
    if event.get('operation') in ('mask', 'and', 'or'):
        golden.append((0, int(event['offset'], 0), int(event['value'], 0)))
    elif event.get('operation') == 'delay':
        golden.append((1, 0, event['microseconds']))
golden += [(0, 0x30, 0)]  # Additional reversible selector restoration.
late = json.loads((root / 'build/displayport-kernelcache/atc-dpxbar-native-late-stop-1.json').read_text())
assert late['stale_stop_status']
assert any(e.get('operation') == 'stop_status_warning' for e in late['events'])
assert late['final_registers'] == reference['final_registers']
assert [e for e in late['events'] if e.get('operation') in ('mask', 'and', 'or', 'delay')] == [
    e for e in reference['events'] if e.get('operation') in ('mask', 'and', 'or', 'delay')]
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define __iomem
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define pr_info(...) do {if(0)printf(__VA_ARGS__);} while(0)
#define pr_err(...) do {} while(0)
static unsigned char regs[0x1000];
static int lost_write, stuck_status, writes, events;
static bool late_status;
struct event {u32 kind, offset, value;};
static struct event trace[128];
static void event(u32 k,u32 o,u32 v) {assert(events<128);trace[events++]=(struct event){k,o,v};}
static u32 readl(const void *p) {
 size_t off=(const unsigned char *)p-regs;assert(off<sizeof(regs) && !(off&3));
 static const u32 sources[]={0,4,0xc,0x10,0x14,0x1c,0x20,0x24};
 if((int)off==stuck_status)return 0xffffffff;
 if(late_status && (off==0x800 || off==0x808 || off==0x814)) {
  u32 gate=off==0x800?4:off==0x808?0x14:0x24;
  u32 bit=off==0x814?0x100:4, v;memcpy(&v,regs+gate,4);
  if(!(v&bit))return bit;
 }
 if(off>=0x800 && off<0x820)off=sources[(off-0x800)/4];
 u32 v;memcpy(&v,regs+off,4);return v;
}
static void setreg(u32 off,u32 v) {memcpy(regs+off,&v,4);}
static void writel(u32 v,void *p) {
 size_t off=(unsigned char *)p-regs;assert(off<0x38 && !(off&3));writes++;
 event(0,off,v);
 if((int)off==lost_write)return;
 setreg(off,v);
}
static void udelay(u32 n) {event(1,0,n);}
'''
program += (root / 'tools/m4-dcpext-probe/dcpext_dpxbar.h').read_text().replace(
    '#include \"../../linux/linux-asahi/drivers/mux/apple-display-crossbar-t8132.h\"',
    (root / 'linux/linux-asahi/drivers/mux/apple-display-crossbar-t8132.h').read_text())
program += 'static const struct event golden[]={' + ','.join(
    '{' + ','.join(str(x) + 'u' for x in e) + '}' for e in golden) + '};\n'
reference4 = json.loads((root / 'linux/linux-asahi/drivers/gpu/drm/apple/m4-dcpext/qualification/dcpext1-selector4-native.json').read_text())
assert reference4['source'] == '0x8020' and reference4['destination'] == '0x8000'
golden4 = []
for event in reference4['events']:
    if event.get('operation') in ('mask', 'and', 'or'):
        golden4.append((0, int(event['offset'], 0), int(event['value'], 0)))
    elif event.get('operation') == 'delay':
        golden4.append((1, 0, event['microseconds']))
golden4.append((0, 0x30, 0))
program += 'static const struct event golden4[]={' + ','.join('{' + ','.join(str(x)+'u' for x in e) + '}' for e in golden4) + '};\n'
program += r'''
static void reset(void) {
 memset(regs,0,sizeof(regs));setreg(4,0x1ff);setreg(0x14,0x1ff);setreg(0x24,0x111);
 session_dpxbar=regs;session_dpxbar_owned=true;session_dpxbar_selected=false;session_xbar.selector=0;
 writes=events=0;lost_write=stuck_status=-1;
 late_status=false;
}
int main(void) {
 reset();assert(!dpxbar_up() && session_dpxbar_selected);
 assert(readl(regs+0x30)==0x200200 && readl(regs)==4);
 assert(!dpxbar_down() && !session_dpxbar_selected && !dpxbar_check_idle());
 assert((size_t)events==ARRAY_SIZE(golden));
 for(int i=0;i<events;i++) {
  if(memcmp(&trace[i],&golden[i],sizeof(trace[i]))) {
   fprintf(stderr,"event %d got=%x,%x,%x want=%x,%x,%x\n",i,
    trace[i].kind,trace[i].offset,trace[i].value,golden[i].kind,golden[i].offset,golden[i].value);
   assert(0);
  }
 }
 puts("PASS: actual selector2 connect/up/down C matches native writes/delays, plus explicit final restoration");
 for(unsigned i=0;i<14;i++) {
  reset();setreg(dpxbar_offsets[i],readl(regs+dpxbar_offsets[i])^1);
  assert(dpxbar_up()==-EBUSY && !writes && !session_dpxbar_selected);
 }
 for(int off=0;off<0x38;off+=4) {
  if(off==0x10 || off==0x20)continue;
  reset();lost_write=off;assert(dpxbar_up()<0 && session_dpxbar_selected);
 }
 puts("PASS: non-idle controls refuse startup; lost selector/clock writes are detected without clearing ownership");
 reset();setreg(0x34,0x100);assert(!dpxbar_check_idle());
 assert(!dpxbar_up() && !dpxbar_down() && readl(regs+0x34)==0x100);
 reset();assert(!dpxbar_up());setreg(0x34,0x200);
 assert(dpxbar_down()==-EIO && session_dpxbar_selected);
 for(int i=0;i<3;i++) {
  static const int status[]={0x800,0x808,0x814};
  reset();assert(!dpxbar_up());stuck_status=status[i];
  assert(dpxbar_down()==-EIO && session_dpxbar_selected);
 }
 reset();assert(!dpxbar_up());late_status=true;
 assert(!dpxbar_down() && !session_dpxbar_selected && !dpxbar_check_idle());
 puts("PASS: only native parked bit8 is admitted; unrelated residual enable or failed clock-stop status retains recovery obligation");
 reset();session_xbar.selector=4;
 assert(!dpxbar_up() && readl(regs+0x30)==0x400400 && readl(regs)==16);
 assert(!dpxbar_down() && !session_dpxbar_selected);
 assert((size_t)events==ARRAY_SIZE(golden4));
 for(int i=0;i<events;i++)assert(!memcmp(&trace[i],&golden4[i],sizeof(trace[i])));
 reset();session_xbar.selector=32;assert(dpxbar_up()==-EINVAL && !writes);
 puts("PASS: DCPEXT1 selector4 writes/delays match independent native execution; invalid selectors never write");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='displayport-dpxbar-') as tmp:
    directory = Path(tmp)
    (directory / 'test.c').write_text(program)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-I', str(root / 'linux/linux-asahi/drivers/mux'), str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)
