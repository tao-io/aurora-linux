#![allow(dead_code)]
#[path="../g16_render.rs"] mod g16_render;
#[path="../g16_compute.rs"] mod g16_compute;
#[path="../g16_render_command.rs"] mod g16_render_command;
use g16_render_command::*;
fn word(b: &[u8], o: usize) -> u32 { u32::from_le_bytes(b[o..o+4].try_into().unwrap()) }
fn main() {
 let raw=std::fs::read(std::env::args().nth(1).expect("captured TA sequence path")).unwrap();
 let captured: &[u8;0x300]=raw[..0x300].try_into().unwrap();
 assert_eq!(captured.len(),0x300);
 // Instruction-derived J613 dispatch of the actual G49 bytes fails on
 // the word immediately after StartTA, matching the retained Cstr crash.
 assert_eq!(word(captured,0)&0x3f,5);
 assert_eq!(word(captured,0x1c8)&0x3f,0);
 let mut b=*captured;
 assert_eq!(j613_tiling_program(&mut b).unwrap(),0x2f4);
 let mut cursor=0;
 for (op,stride) in [(5,0x1c8),(3,0x48),(1,4),(3,0x48),(6,0x94),(2,4)] {
  assert_eq!(word(&b,cursor)&0x3f,op,"J613 opcode at {cursor:#x}");
  cursor+=stride;
 }
 assert_eq!(cursor,0x2f4);
 assert_eq!(word(&b,0x25c+0x80),(-0x25ci32) as u32);
 assert_eq!(b[..0x1c8],captured[..0x1c8]);
 assert_eq!(b[0x1c8..0x210],captured[0x1cc..0x214]);
 assert_eq!(b[0x214..0x25c],captured[0x21c..0x264]);
 // Finalize's external pointers and self UMA-header pointer retain values.
 assert_eq!(b[0x25c..0x2dc],captured[0x268..0x2e8]);
 assert_eq!(b[0x2e0..0x2f4],captured[0x2ec..0x300]);
 assert!(b[0x2f4..].iter().all(|v|*v==0));
 for off in [0,0x1cc,0x218,0x21c,0x268,0x2e8,0x2fc] {
  let mut invalid=*captured;invalid[off]^=1;let saved=invalid;
  assert!(j613_tiling_program(&mut invalid).is_err());assert_eq!(invalid,saved);
 }
 let mut short=*captured;let saved=short;
 assert!(j613_tiling_program(&mut short[..0x2ff]).is_err());assert_eq!(short,saved);
 // A second conversion and already shortened shape must be rejected.
 let saved=b;assert!(j613_tiling_program(&mut b).is_err());assert_eq!(b,saved);
 println!("PASS: captured G49 failure replayed; six J613 dispatch strides retire offline, pointers retained, restart corrected, invalid shapes unchanged");
}
