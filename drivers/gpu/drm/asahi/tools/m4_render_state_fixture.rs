#![allow(dead_code)]
#[path="../g16_render.rs"] mod g16_render;
#[path="../g16_render_state.rs"] mod g16_render_state;
use g16_render::*;
use g16_render_state::*;
use std::io::{self, Write};
fn main() {
 let args: Vec<String> = std::env::args().collect();
 let layers: u16 = args.get(1).map(|s| s.parse().unwrap()).unwrap_or(1);
 let empty = args.get(2).map(|s| s == "1").unwrap_or(true);
 for (w,h,utile,base) in [(4,4,Utile::Pixels32,0x3000000000u64),
  (2560,1664,Utile::Pixels32,0x7000000000),(257,139,Utile::Pixels16,0x8000000000)] {
  let g = Geometry::new_layered(w,h,utile,utile,1,layers).unwrap();
  let s = State { geometry:g,parameter_buffer_id:0,vdm:base+0x10000,tilemap:base+0x20000,tpc:base+0x30000,
   unknown:base+0x40000,preemption:[base+0x50000,base+0x60000,base+0xe0000],scratch:base+0x70000,
   merge_upper:[0x3addb3d9;2],tilebuffer_blocks:2,
   scene_list:base+0xf0000,auxiliary:base+0x80000,multisample:0x88,ppp:0x201,
   background:Program{address:0x10000004000,resources:0x300000012},eot:Program{address:0x10000008000,resources:0x500000014},
   partial_background:Program{address:0x1000000c000,resources:0x700000016},partial_eot:Program{address:0x10000010000,resources:0x900000018},
   depth_bias:base+0x90000,scissor:base+0xa0000,query:base+0xb0000,
   depth:DepthStencil{base:base+0xc0000,stride:0x45678},stencil:DepthStencil{base:base+0xd0000,stride:0x98765},
   depth_dimensions:0x12345678,zls_control:0xabcde1234,depth_clear:0x3f800000,stencil_clear:0x307,process_empty_tiles:empty,integer_depth_bias:false };
  for stage in [Stage::Tiling,Stage::Fragment] {
   let mut registers = [Register::default();128];
   let (n,mode) = g16_render_state::registers(&mut registers,stage,&s,0x16001234).unwrap();
   let mut out = [0;0xca0];
   write_registers(&mut out,stage,base,&registers[..n]).unwrap();
   mirrors(&mut out,stage,&s,0xfffffc2004000000,0xfffffc2008000000,0xfffffc200c000000,mode).unwrap();
   io::stdout().write_all(&out).unwrap();
  }
 }
}
