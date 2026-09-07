// SPDX-License-Identifier: GPL-2.0-only
/* ctypes adapter for the optional native-producer differential test. */
#include "g16_render_ta.h"
#include "g16_render_fragment.h"
#include "g16_render_geometry.h"
int ta(u8 *out, const u8 *d, const struct g16_render_ta_platform *p, u64 *mode) {
 struct g16_render_register r[128];
 int n=g16_render_ta_registers(r,128,d,0x1428,p,0x12345678,mode);
 for(int i=0;i<n;i++){g16_render_put(out+i*12,r[i].reg|r[i].flag,4);g16_render_put(out+i*12+4,r[i].value,8);}return n;
}
int fragment(u8 *out, const u8 *d, const struct g16_render_fragment_platform *p, u64 *mode) {
 struct g16_render_register r[128];
 int n=g16_render_fragment_registers(r,128,d,0x1428,p,0x12345678,mode);
 for(int i=0;i<n;i++){g16_render_put(out+i*12,r[i].reg|r[i].flag,4);g16_render_put(out+i*12+4,r[i].value,8);}return n;
}
int geometry(struct g16_render_geometry *g, u32 w,u32 h,u32 x,u32 y,u32 n){return g16_render_geometry_single(g,w,h,x,y,n);}
int pb_hw(u8 *out, u64 va,u32 count,u32 r,u32 w,u8 wrap,u8 valid){return g16_render_pack_pb_hw(out,16,va,count,r,w,wrap,valid);}
