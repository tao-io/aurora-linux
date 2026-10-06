#![allow(dead_code)]
#[path="../g16_render.rs"] mod g16_render;
#[path="../g16_compute.rs"] mod g16_compute;
#[path="../g16_render_command.rs"] mod g16_render_command;
use g16_render_command::*;
fn main() {
 for id in [0, 1, 15, 31] {
  let base=0xfffffc20004d4000u64 + (id as u64)*0x100000;
  let a=ParameterManager {pages_fw:base+0x10000,pages_gpu:0x71040c0000,
   blocks:base+0x18000,ring:base+0x20000,counter:base+0x28000,discard:0x180000,
   list_bytes:512,pages:128,block_capacity:64,write:32,read:0,
   max_pages:128,min_pages:128,id};
  let mut b=[0xa5;0xc0];
  parameter_manager_for_layout(&mut b,a,ParameterLayout::for_chip(0x8122)).unwrap();
  let q=|o|u64::from_le_bytes(b[o..o+8].try_into().unwrap());
  let w=|o|u32::from_le_bytes(b[o..o+4].try_into().unwrap());
  // Actual J613 c000 instruction offsets, independently retained in disassembly.
  // 31140: PBDesc id; 3117c/31278: aliases; 311b0: byte span;
  // 311bc/311c0: block array + capacity; 31170: ring pointer;
  // 311d0: cursors; 311e4: block size; 31460: compact discard.
  assert_eq!(w(8),id); assert_eq!(q(0x1c),a.pages_fw); assert_eq!(q(0x24),a.pages_gpu);
  assert_eq!(w(0x2c),512); assert_eq!(q(0x40),a.blocks); assert_eq!(w(0x34),64);
  assert_eq!(q(0x48),a.ring); assert_eq!(w(0x38),32); assert_eq!(w(0x3c),0);
  assert_eq!(w(0x54),0x20000); assert_eq!(q(0x80),0x180000);
  assert_eq!(w(0x78),128); assert_eq!(w(0x7c),128);
  assert_eq!(w(ParameterLayout::for_chip(0x8122).last_page()),127);
  // The old producer recreates the exact cross-pointer failure class.
  let mut old=[0;0xc0];parameter_manager(&mut old,a).unwrap();
  let bad=u64::from_le_bytes(old[0x48..0x50].try_into().unwrap());
  assert_eq!(bad,((a.ring & 0xffffffff)<<32)|(a.blocks>>32));
  assert_ne!(bad,a.ring);
  // Address rejection must not partially publish a descriptor.
  let mut invalid=a;invalid.ring=0;
  let before=b;
  assert!(parameter_manager_for_layout(&mut b,invalid,ParameterLayout::J613).is_err());
  assert_eq!(before,b);
  assert!(parameter_manager_for_layout(&mut b[..0xbf],a,ParameterLayout::J613).is_err());
 }
 println!("PASS: four descriptor IDs, J613 firmware reads, old cross-pointer reproduced, invalid inputs rejected atomically");
}
