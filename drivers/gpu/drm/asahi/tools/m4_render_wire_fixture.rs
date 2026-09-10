// SPDX-License-Identifier: GPL-2.0-only
#![allow(dead_code)]
#[path = "../g16_render.rs"] mod g16_render;
#[path = "../g16_render_command.rs"] mod g16_render_command;
use std::io::Write;
use g16_render_command::*;
fn main() {
    let b = 0xfffffc2001000000;
    let mut out = std::io::stdout();
    for stage in [g16_render::Stage::Tiling, g16_render::Stage::Fragment] {
        let a = Args { command:b, sku:b+0x4000, queue:b+0x8000, stats:b+0xc004,
            pb_slot:b+0x10000, manager:b+0x14000, pb_aux:b+0x18000, notifier:b+0x1c000,
            shared_stamp:b+0x20000, stamp_address:b+0x24000, uma:b+0x28000,
            uma_aux:0x7003000000, context:3, generation:2, counter:0x100000001,
            uuid:0x1234, stamp:0x300, event_slot:0x71, pb_id:2, pass_id:4 };
        let mut command = [0xa5; 0xca0]; let mut sku = [0xa5; 0x300];
        encode(&mut command,&mut sku,stage,a).unwrap();
        // Firmware engine statistics and queue pointers must survive both
        // start and finalize; the two engines use different packed offsets.
        let ptr = |off| u64::from_le_bytes(sku[off..off+8].try_into().unwrap());
        let (stats_offsets, queue_offsets) = match stage {
            g16_render::Stage::Tiling => ([0x2c, 0x1e4], [0x34, 0x1ec]),
            g16_render::Stage::Fragment => ([0x24, 0x220], [0x4c, 0x240]),
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
    let a = ParameterManager { pages_fw:b+0x10000,pages_gpu:0x7002000000,
        blocks:b+0x14000,ring:b+0x18000,counter:b+0x1c000,discard:0,
        list_bytes:0x4000,pages:128,block_capacity:64,write:32,read:0,
        max_pages:256,min_pages:128,id:2 };
    let mut manager=[0xa5;0xc0]; parameter_manager(&mut manager,a).unwrap();
    out.write_all(&manager).unwrap();
    let mut init=[0xa5;0x20];init_parameter_buffer(&mut init,3,2,0,32,b+0x14000,0x300).unwrap();
    out.write_all(&init).unwrap();
}
