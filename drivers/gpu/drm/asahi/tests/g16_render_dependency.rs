// SPDX-License-Identifier: GPL-2.0-only
// rustc --edition=2021 --test tests/g16_render_dependency.rs -o /tmp/g16-render-dependency
#![allow(dead_code)]
#[path = "../g16_render.rs"]
mod g16_render;
#[path = "../g16_render_command.rs"]
mod g16_render_command;

#[test]
fn native_start_and_command_counters_agree_without_truncation() {
    // Matched 25G83 capture193311: TA counter2 / fragment counter1 occur
    // both at RunCommand+4 and StartTA+1b8 / StartFragment+1d8.
    use g16_render::Stage;
    use g16_render_command::Args;
    let p = 0xfffffc2012340000;
    for (stage, offset, counter) in [
        (Stage::Tiling, 0x1b8, 2), (Stage::Fragment, 0x1d8, 1),
        (Stage::Tiling, 0x1b8, 0x123456789abcdef0),
        (Stage::Fragment, 0x1d8, 0x123456789abcdef0),
    ] {
        let a = Args {
            command:p,sku:p+0x4000,queue:p+0x8000,stats:p+0xc000,
            pb_slot:p,manager:p,pb_aux:p,notifier:p,shared_stamp:p,
            stamp_address:p,uma:p,uma_aux:0x7100010000,
            context:2,generation:1,counter,uuid:42,stamp:0x100,event_slot:0x70,
            pb_id:0,pass_id:0,
        };
        let mut command = [0;0xca0];
        let mut sku = [0;0x300];
        g16_render_command::encode(&mut command,&mut sku,stage,a).unwrap();
        assert_eq!(&command[4..12], &counter.to_le_bytes());
        assert_eq!(&sku[offset..offset+8], &counter.to_le_bytes());
        assert_eq!(&sku[offset+8..offset+12], &0x70u32.to_le_bytes());
        // Native 193311/193422: all three fragment mode mirrors and the
        // vertex finalizer are 1. Mode-2 command flags remain clear.
        if stage == Stage::Fragment {
            assert_eq!(command[0xc84], 1);
            assert_eq!(sku[0x1e4], 1);
            assert_eq!(sku[0x1f0 + 0xb1], 1);
            assert_eq!(&command[0xbec..0xbf0], &[0;4]);
        } else {
            assert_eq!(sku[0x1d0 + 0x8d], 1);
            assert_eq!(&command[0x8a4..0x8a8], &[0;4]);
        }
    }
}

#[test]
fn m4_slot_dependency_wire_record() {
    // Independent M4 producer/consumer contract: two stamp aliases, u32
    // target stamp, event slot, own DAG stamp, then zero optional controls.
    let expected: [u32; 16] = [
        4, 0x12340408, 0xfffffc20, 0x12340408, 0xfffffc20, 0x2300,
        0, 0, 0x70, 0x100, 0, 0, 0, 0, 0, 0,
    ];
    let mut record = [0xa5; 0x48];
    g16_render_command::render_dependency(&mut record, 0xfffffc2012340408,
        0x70, 0x2300, 0x100).unwrap();
    for (word, value) in record[..0x40].chunks_exact(4).zip(expected) {
        assert_eq!(word, value.to_le_bytes());
    }
    assert_eq!(&record[0x40..], &[0xa5; 8]);
}

#[test]
fn reject_external_or_unmapped_wait_before_writing() {
    for (address, slot, wait, own) in [
        (0x7100000408, 0x70, 0x100, 0x100),
        (0xfffffc2012340409, 0x70, 0x100, 0x100),
        (0xfffffc2012340408, 0x80, 0x100, 0x100),
        (0xfffffc2012340408, 0x70, 0, 0x100),
        (0xfffffc2012340408, 0x70, 0x100, 0),
    ] {
        let mut record = [0xa5; 0x40];
        assert_eq!(g16_render_command::render_dependency(&mut record,
            address, slot, wait, own), Err(g16_render::BuildError::Address));
        assert_eq!(record, [0xa5; 0x40]);
    }
    let mut short = [0xa5; 0x3f];
    assert_eq!(g16_render_command::render_dependency(&mut short,
        0xfffffc2012340408, 0x70, 0x100, 0x100),
        Err(g16_render::BuildError::BufferTooSmall));
    assert_eq!(short, [0xa5; 0x3f]);
}
