// SPDX-License-Identifier: GPL-2.0-only
// rustc --edition=2021 --test tests/g16_render_native_state.rs -o /tmp/g16-native-state
#![allow(dead_code)]
#[path = "../g16_render.rs"] mod g16_render;
#[path = "../g16_render_state.rs"] mod g16_render_state;
use g16_render::{Geometry, Register, Stage, Utile};
use g16_render_state::{State, Program, DepthStencil};

#[test]
fn native_m4_merge_dimensions_and_tilebuffer_contract() {
    // Matched J713/25G83 Metal mode-2 captures, 2026-09-10 19:33/19:34 UTC.
    // The merge words are IEEE float encodings supplied by userspace, not
    // the geometry's macro-tile count. Addresses are independent test owners.
    for (width,merge) in [(1024,0x3addb3d9),(128,0x3c5db3d9)] {
        for blocks in [0,2,4,16] {
            let a=0x1000010000;
            let s=State {
                geometry:Geometry::new(width,width,Utile::Pixels32,Utile::Pixels32,1).unwrap(),
                vdm:a,tilemap:a,tpc:a,unknown:a,preemption:[a,a],scratch:a,auxiliary:a,
                multisample:0x88,ppp:0,merge_upper:[merge,merge],tilebuffer_blocks:blocks,
                background:Program::default(),eot:Program::default(),
                partial_background:Program::default(),partial_eot:Program::default(),
                depth_bias:0,scissor:a,query:0,depth:DepthStencil::default(),stencil:DepthStencil::default(),
                depth_dimensions:0,zls_control:0,depth_clear:0x3f800000,stencil_clear:0x300,
                process_empty_tiles:true,
            };
            let mut regs=[Register::default();128];
            let (n,_)=g16_render_state::registers(&mut regs,Stage::Fragment,&s,42).unwrap();
            for (address,value) in [(0x15130,merge as u64),(0x15138,merge as u64),
                (0x15210,((width as u64)<<32)|width as u64),(0x10050,blocks as u64),
                (0x101e8,0x1c),(0x1a0f8,8)] {
                let matches:Vec<_>=regs[..n].iter().filter(|r|r.offset==address).collect();
                assert_eq!(matches.len(),1);
                assert!(matches[0].flagged);
                assert_eq!(matches[0].value,value);
            }
            let (n,_)=g16_render_state::registers(&mut regs,Stage::Tiling,&s,42).unwrap();
            for (address,value) in [(0x101e0,0x1c),(0x1a0f0,8)] {
                assert!(regs[..n].iter().any(|r|r.offset==address && r.flagged && r.value==value));
            }
        }
    }
}
