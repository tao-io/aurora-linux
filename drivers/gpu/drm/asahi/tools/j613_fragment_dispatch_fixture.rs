#![allow(dead_code)]
#[path="../g16_render.rs"] mod g16_render;
#[path="../g16_compute.rs"] mod g16_compute;
#[path="../g16_render_command.rs"] mod g16_render_command;
use g16_render_command::*;
fn word(b:&[u8],o:usize)->u32{u32::from_le_bytes(b[o..o+4].try_into().unwrap())}
fn main(){
 let raw=std::fs::read(std::env::args().nth(1).unwrap()).unwrap();
 let captured:&[u8;0x380]=raw[..0x380].try_into().unwrap();
 // StartFragment sets x27=x23+4, then adds 0x1e0 to x27.
 // WFI uses x27=x23 from the dispatch loop, then adds 4.
 assert_eq!(word(captured,0),7);assert_eq!(word(captured,4+0x1e0),1);
 assert_eq!(word(captured,4+0x1e0+4),0);
 let mut b=*captured;assert_eq!(j613_fragment_start(&mut b).unwrap(),0x378);
 let mut cursor=0;
 for (op,header,payload) in [(7,4,0x1e0),(3,4,0x48),(1,0,4),(3,4,0x48)] {
  assert_eq!(word(&b,cursor)&63,op,"opcode {cursor:#x}");cursor+=header+payload;
 }
 assert_eq!(cursor,0x280);assert_eq!(word(&b,cursor),8);
 // Only start-tail removal, relative restart, and owned zero tail change.
 let mut expected=[0u8;0x380];expected[..0x1e4].copy_from_slice(&captured[..0x1e4]);
 expected[0x1e4..0x378].copy_from_slice(&captured[0x1ec..]);
 expected[0x320..0x324].copy_from_slice(&(-0x280i32).to_le_bytes());assert_eq!(b,expected);
 for off in [0,0x1ec,0x238,0x23c,0x288,0x328,0x344] {
  let mut invalid=*captured;invalid[off]^=1;let saved=invalid;
  assert!(j613_fragment_start(&mut invalid).is_err());assert_eq!(invalid,saved);
 }
 let saved=b;assert!(j613_fragment_start(&mut b).is_err());assert_eq!(b,saved);
 let mut short=*captured;let saved=short;assert!(j613_fragment_start(&mut short[..0x37f]).is_err());assert_eq!(short,saved);
 println!("PASS: captured G49 fragment cursor failure replayed; opcode plus payload strides reach FinalizeFragment; exact start-tail/restart change; invalid shapes unchanged. Finalize execution remains unqualified.");
}
