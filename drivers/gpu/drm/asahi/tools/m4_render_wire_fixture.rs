// SPDX-License-Identifier: GPL-2.0-only
#![allow(dead_code)]
#[path = "../g16_render.rs"] mod g16_render;
#[path = "../g16_compute.rs"] mod g16_compute;
#[path = "../g16_render_command.rs"] mod g16_render_command;
use std::io::Write;
use g16_render_command::*;
fn main() {
    let variant: u64 = std::env::args().nth(1).map(|v| v.parse().unwrap()).unwrap_or(0);
    assert!(variant < 64);
    let b = 0xfffffc2001000000 + variant * 0x100000;
    let mut out = std::io::stdout();
    for stage in [g16_render::Stage::Tiling, g16_render::Stage::Fragment] {
        let mut a = Args { command:b, sku:b+0x4000, queue:b+0x8000, stats:b+0xc004,
            pb_slot:b+0x10000, manager:b+0x14000, pb_aux:b+0x18000, notifier:b+0x1c000,
            shared_stamp:b+0x20000, stamp_address:b+0x24000, uma:b+0x28000,
            uma_aux:0x7003000000, context:3, generation:2, counter:0x100000001,
            timestamp_storage:b+0x30000, uuid:0x1234, stamp:0x300, event_slot:0x71,
            fragment_slot:0x71, fragment_stamp:0x300, pb_id:2, pass_id:4 };
        if variant != 0 {
            a.context = variant as u32; a.generation = variant as u8;
            a.counter = (variant << 32) | (u32::MAX as u64 - variant);
            a.uuid = u32::MAX - variant as u32; a.stamp = (variant as u32 + 1) << 8;
            a.event_slot = variant as u32; a.fragment_slot = 127 - variant as u32;
            a.fragment_stamp = a.stamp + 0x100; a.pass_id = variant as u32;
        }
        let mut command = [0xa5; 0xca0]; let mut sku = [0xa5; 0x380];
        encode(&mut command,&mut sku,stage,a).unwrap();
        // Firmware engine statistics and queue pointers must survive both
        // start and finalize; the two engines use different packed offsets.
        let ptr = |off| u64::from_le_bytes(sku[off..off+8].try_into().unwrap());
        let (stats_offsets, queue_offsets) = match stage {
            g16_render::Stage::Tiling => ([0x2c, 0x27c], [0x34, 0x284]),
            g16_render::Stage::Fragment => ([0x24, 0x2b8], [0x4c, 0x2d8]),
        };
        let control = if stage == g16_render::Stage::Tiling { 0x48 } else { 0x60 };
        assert_eq!(&sku[control..control+4], &1u32.to_le_bytes());
        for off in stats_offsets { assert_eq!(ptr(off), a.stats); }
        for off in queue_offsets { assert_eq!(ptr(off), a.queue); }
        out.write_all(&command).unwrap();out.write_all(&sku).unwrap();
        let before = command; let before_sku = sku;
        let mut invalid = a; invalid.context=64;
        assert!(encode(&mut command,&mut sku,stage,invalid).is_err());
        assert_eq!(command,before);assert_eq!(sku,before_sku);
    }
    compute_wire(&mut out, variant);
    let a = ParameterManager { pages_fw:b+0x10000,pages_gpu:0x7002000000,
        blocks:b+0x14000,ring:b+0x18000,counter:b+0x1c000,discard:0,
        list_bytes:0x4000,pages:128,block_capacity:64,write:32,read:0,
        max_pages:256,min_pages:128,id:2 };
    let mut manager=[0xa5;0xc0]; parameter_manager(&mut manager,a).unwrap();
    out.write_all(&manager).unwrap();
    let mut init=[0xa5;0x20];init_parameter_buffer(&mut init,3,2,0,32,b+0x14000,0x300).unwrap();
    out.write_all(&init).unwrap();
}

// Execute the actual compute producer with the same fixture for both source
// versions. Raw offsets here intentionally remain an independent baseline.
fn compute_wire(out: &mut impl Write, variant: u64) {
    use g16_compute::{Addresses, Control, Notification, UmaEngine};
    let b = 0xffff_fc20_4000_0000 + variant * 0x100000;
    let g = 0x7100000000 + variant * 0x100000;
    let a = Addresses { command:b, command_gpu:g, cdm_gpu:g+0x4000,
        preemption_gpu:g+0x8000, metrics:b+0x10000, auxiliary_gpu:g+0x10000,
        work_queue:b+0x14000, stats:b+0x18000, context_store_gpu:g+0x20000,
        pool_state_gpu:g+0x40000, page_list_gpu:g+0x60000, counter:b+0x1c000,
        context:variant as u32, generation:variant as u8,
        stamp:0xffffff00-(variant as u32*0x100), event_slot:(variant%4) as u8 };
    let c = Control {base:a.cdm_gpu, end:a.cdm_gpu+0x130,
        usc_base:if variant%2==0 {0}else{0x7200000000}, wide_visibility:false};
    let n = Notification {address:b+0x24000,threshold:b+0x28000,
        stamp:b+0x2c000,fw_stamp:b+0x30000,event_seq:(variant<<32)|0xfffffff0};
    for pages in [0,512,8192] {
        for notification in [None,Some(n)] {
            let mut bytes=[0xa5;0x1008];
            g16_compute::encode_dispatch(&mut bytes,a,16384,pages,c,notification).unwrap();
            out.write_all(&bytes).unwrap();
            for count in [0,1,8] {
                let mut seq=[0x5a;0x600];seq[..0x300].copy_from_slice(&bytes[0x900..0xc00]);
                let pointers: [u64;8]=core::array::from_fn(|i|b+0x34000+i as u64*16);
                let size=g16_compute::timestamp_copies(&mut seq,&pointers[..count]).unwrap();
                out.write_all(&(size as u64).to_le_bytes()).unwrap();out.write_all(&seq).unwrap();
            }
            let mut seq=bytes[0x900..0xc00].to_vec();
            let size=g16_compute::omit_timestamps(&mut seq,0x1bc,0x300,0x64).unwrap();
            out.write_all(&(size as u64).to_le_bytes()).unwrap();out.write_all(&seq).unwrap();
        }
        for engine in [UmaEngine::Compute,UmaEngine::Render] {
            let mut bytes=[0xa5;0xa8];
            g16_compute::encode_uma(&mut bytes,a.pool_state_gpu,16384,pages,a.page_list_gpu,a.counter,engine).unwrap();
            out.write_all(&bytes).unwrap();
        }
    }
    let mut bytes=[0xa5;0x1000];let mut bad=a;bad.command_gpu|=1;
    assert!(g16_compute::encode_dispatch(&mut bytes,bad,16384,512,c,None).is_err());
    assert_eq!(bytes,[0xa5;0x1000]);
    // Invalid slots are historically rejected after prefix mutation. Compare
    // the entire result instead of silently changing that contract in cleanup.
    bad=a;bad.event_slot=4;
    assert!(g16_compute::encode_dispatch(&mut bytes,bad,16384,512,c,None).is_err());
    assert_ne!(bytes,[0xa5;0x1000]);out.write_all(&bytes).unwrap();
}
