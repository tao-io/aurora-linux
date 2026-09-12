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
                vdm:a,tilemap:a,tpc:a,unknown:a,preemption:[a,a,a],scratch:a,auxiliary:a,scene_list:a,
                multisample:0x88,ppp:0,merge_upper:[merge,merge],tilebuffer_blocks:blocks,
                background:Program::default(),eot:Program::default(),
                partial_background:Program::default(),partial_eot:Program::default(),
                depth_bias:0,scissor:a,query:0,depth:DepthStencil::default(),stencil:DepthStencil::default(),
                depth_dimensions:0,zls_control:0,depth_clear:0x3f800000,stencil_clear:0x300,
                process_empty_tiles:true,
            };
            // Independent native wire bytes: UMA pointer, maintenance flag,
            // optional size and auxiliary pointer use packed offsets.
            for (stage, base) in [(Stage::Tiling,0x8fe),(Stage::Fragment,0xc46)] {
                let mut cmd=[0;0xca0];
                g16_render_state::mirrors(&mut cmd,stage,&s,
                    0xfffffc2010000000,0xfffffc2012340000,0xfffffc2012350000,0).unwrap();
                assert_eq!(&cmd[base..base+8],&0xfffffc2012340000u64.to_le_bytes());
                assert_eq!(cmd[base+8],1);
                assert_eq!(&cmd[base+9..base+17],&[0;8]);
                assert_eq!(&cmd[base+17..base+25],&0xfffffc2012350000u64.to_le_bytes());
            }
            let mut regs=[Register::default();128];
            let (n,_)=g16_render_state::registers(&mut regs,Stage::Fragment,&s,42).unwrap();
            for (address,value) in [(0x15130,merge as u64),(0x15138,merge as u64),
                (0x15210,((width as u64)<<32)|width as u64),(0x10050,blocks as u64),
                (0xa5a8,0x6000400020),(0xd428,0x200000001),
                (0x101e8,0x1c),(0x1a0f8,8),(0x15020,0xc000),(0x15048,0x100000)] {
                let matches:Vec<_>=regs[..n].iter().filter(|r|r.offset==address).collect();
                assert_eq!(matches.len(),1);
                assert!(matches[0].flagged);
                assert_eq!(matches[0].value,value);
            }
            assert!(regs[..n].iter().any(|r|r.offset==0x1ca28 && !r.flagged && r.value==0x10000));
            let (n,_)=g16_render_state::registers(&mut regs,Stage::Tiling,&s,42).unwrap();
            for (offset,flagged) in [(0x1ca30,false),(0x16c38,true)] {
                assert!(regs[..n].iter().any(|r|r.offset==offset && r.flagged==flagged && r.value==0x10000));
            }
            assert!(regs[..n].iter().any(|r|r.offset==0x1c950 && !r.flagged
                && r.value==0x0004000000010000));
            for (address,value) in [(0xa5a0,0x6000400020),(0xd418,0x200000001),
                (0x101e0,0x1c),(0x1a0f0,8)] {
                assert!(regs[..n].iter().any(|r|r.offset==address && r.flagged && r.value==value));
            }
        }
    }
}

#[test]
fn sample_geometry_keeps_spatial_fields_and_replaces_count() {
    for (x,y,spatial) in [(Utile::Pixels32,Utile::Pixels32,0xa000),
                         (Utile::Pixels32,Utile::Pixels16,0x6000),
                         (Utile::Pixels16,Utile::Pixels16,0x5000)] {
        let mut g=Geometry::new(64,64,x,y,1).unwrap();
        let allocation=(g.tilemap_bytes,g.tpc_bytes);
        for (samples,bits) in [(4,2),(2,1),(1,0)] {
            g.set_samples(samples).unwrap();
            assert_eq!(g.utile_config,spatial|bits);
            assert_eq!((g.tilemap_bytes,g.tpc_bytes),allocation);
        }
        for invalid in [0,3,8,255] {
            assert!(g.set_samples(invalid).is_err());
            assert_eq!(g.utile_config,spatial);
        }
    }
}
