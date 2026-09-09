// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <assert.h>
#include "../g16_lab/render/g16_render_ta.h"
#include "../g16_lab/render/g16_render_fragment.h"
#include "../g16_lab/render/g16_render_geometry.h"
int main(void) {
 const unsigned dims[][3]={{4,4,32},{2560,1664,32},{257,139,16}};
 const u64 bases[]={0x3000000000,0x7000000000,0x8000000000};
 for(unsigned test=0;test<3;test++) {
  struct g16_render_geometry g;
  assert(!g16_render_geometry_single(&g,dims[test][0],dims[test][1],dims[test][2],dims[test][2],1));
  u64 b=bases[test],c=b-0x1000000000;
  for(unsigned stage=0;stage<2;stage++) {
   u8 d[0x1428]={0},out[0xca0]={0}; struct g16_render_register regs[128]; u64 mode; int n;
#define D(o,v,z) g16_render_put(d+(o),(v),(z))
#define P(o,v,z) g16_render_put(out+(o),(v),(z))
   if(!stage) {
    struct g16_render_ta_platform p={.accelerator_count_4e4=1,.default_d418=1,.pb_slot_word_00=b+0x70000,.accelerator_flags_6a8=0x10000};
    D(0xea8,c+0x10000,8);D(0xf48,c+0x50000,8);D(0xf50,c+0x60000,8);
    D(0xf88,0x003a0012006b0003ULL,8);D(0xf90,1,4);
    D(0x1000,c+0x30000,8);D(0x1010,g.tpc_stride,4);D(0x1018,c+0x20000,8);D(0x1020,g.region_stride,4);
    D(0x1030,c+0x40000,8);D(0x1038,1,4);D(0x1040,c+0x40000,8);
    D(0x10a8,g.utile_config,8);D(0x10b0,0x88,8);D(0xff8,1,1);
    D(0x10b8,g.pixels,4);D(0x10bc,g.screen,4);D(0x10c0,g.x_blocks,4);D(0x10c4,g.y_blocks,4);D(0x10c8,g.blocks,4);D(0x7a4,~0U,4);
    n=g16_render_ta_registers(regs,128,d,sizeof(d),&p,0x16001234,&mode);assert(n>0);
    assert(!g16_render_pack_registers(out,sizeof(out),stage,b,regs,n));
    P(0x760,0xfffffc2004000000ULL,8);P(0x768,g.tpc_bytes,8);P(0x7a8,c+0x50000,8);
    P(0x8fe,0xfffffc2008000000ULL,8);P(0x90f,0xfffffc200c000000ULL,8);P(0x830,mode,8);
   } else {
    struct g16_render_fragment_platform p={.accelerator_count_4e4=1,.disable_spill_registers=1};
    D(0x41c,dims[test][2],4);D(0x420,dims[test][2],4);
    D(0x580,0x300000012ULL,8);D(0x588,0x10000004000ULL,8);D(0x6d8,0x500000014ULL,8);D(0x6e0,0x10000008000ULL,8);
    D(0x5b0,0x700000016ULL,8);D(0x5b8,0x1000000c000ULL,8);D(0x6f8,0x900000018ULL,8);D(0x700,0x10000010000ULL,8);
    D(0x428,b+0x90000,8);D(0x430,b+0xa0000,8);D(0x438,b+0xb0000,8);
    D(0x720,0x3f800000,4);D(0x724,0x307,4);D(0x7e8,0x88,8);D(0x7bd,1,1);D(0x6b0,b+0x80000,8);
    D(0x5d8,0x12345678,8);D(0x5c0,0xabcde1234ULL,8);D(0x5c8,0x04040404,8);
    D(0x5e0,b+0xc0000,8);D(0x5e8,b+0xc0000,8);D(0x5f0,b+0xc0000,8);
    D(0x5f8,b+0xd0000,8);D(0x600,b+0xd0000,8);D(0x608,b+0xd0000,8);
    D(0x640,0x45678,8);D(0x648,0x45678,8);D(0x650,0x98765,8);D(0x658,0x98765,8);
    D(0x7d0,b+0x20000,8);D(0x7d8,b+0x40000,8);D(0x7e0,b+0x40000,8);
    D(0x7f8,1,4);D(0x800,g.isp_blocks,4);D(0x804,g.screen,4);D(0x808,g.x_blocks,4);D(0x80c,g.y_blocks,4);D(0x810,g.blocks,4);
    D(0x820,1,4);D(0x824,g.region_stride,4);D(0x828,g.tiles,4);D(0x890,1,1);D(0x891,1,1);
    n=g16_render_fragment_registers(regs,128,d,sizeof(d),&p,0x16001234,&mode);assert(n>0);
    assert(!g16_render_pack_registers(out,sizeof(out),stage,b,regs,n));
    P(0x40,b+0x20000,8);P(0x50,1,4);P(0x54,g.isp_blocks,4);P(0x60,g.x_blocks,4);P(0x64,g.y_blocks,4);P(0x68,g.blocks,4);P(0x78,g.tiles,4);
    assert(!g16_render_fragment_resume_state(out,sizeof(out),d,sizeof(d)));
    P(0xc46,0xfffffc2008000000ULL,8);P(0xc57,0xfffffc200c000000ULL,8);P(0x9d8,mode,8);
   }
   assert(fwrite(out,1,sizeof(out),stdout)==sizeof(out));
  }
 }
}
