// SPDX-License-Identifier: GPL-2.0-only
// rustc --edition=2021 --test tests/g16_render_dependency.rs -o /tmp/g16-render-dependency
#![allow(dead_code)]
#[path = "../g16_compute.rs"] mod g16_compute;
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
            stamp_address:p,uma:p,uma_aux:0x7100010000,timestamp_storage:p+0x600,
            context:2,generation:1,counter,uuid:42,stamp:0x100,event_slot:0x70,
            fragment_slot:0x71,fragment_stamp:0x432100,pb_id:0,pass_id:0,
        };
        let mut command = [0;0xca0];
        let mut sku = [0;0x400];
        g16_render_command::encode(&mut command,&mut sku,stage,a).unwrap();
        let event_generation = if stage == Stage::Tiling { 0x4c } else { 0x64 };
        assert_eq!(&sku[event_generation..event_generation+4], &[0; 4]);
        assert_eq!(&command[4..12], &counter.to_le_bytes());
        assert_eq!(&sku[offset..offset+8], &counter.to_le_bytes());
        assert_eq!(&sku[offset+8..offset+12], &0x70u32.to_le_bytes());
        // Native 193311/193422: all three fragment mode mirrors and the
        // vertex finalizer are 1. Mode-2 command flags remain clear.
        if stage == Stage::Fragment {
            assert_eq!(command[0xc84], 1);
            assert_eq!(sku[0x1e4], 1);
            assert_eq!(sku[0x288 + 0xb1], 1);
            assert_eq!(&command[0xbec..0xbf0], &[0;4]);
        } else {
            // Firmware's partial-render lookup takes the low seven slot bits
            // and the generation byte at +781, independently of TA's event.
            assert_eq!(&command[0x77c..0x784], &[0x71,0,0,0,0,0x21,0x43,0]);
            assert_eq!(sku[0x268 + 0x8d], 1);
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

#[test]
fn concurrent_parameter_owners_select_disjoint_firmware_descriptors() {
    let p = 0xfffffc2012340000;
    let mut hardware_table = [0u64; 32];
    let mut managers = Vec::new();
    for id in 0..32 {
        let pages = 0x7100000000 + u64::from(id) * 0x4000;
        let mut manager = [0u8; 0xc0];
        g16_render_command::parameter_manager(&mut manager,
            g16_render_command::ParameterManager {
                pages_fw:p, pages_gpu:pages, blocks:p, ring:p, counter:p,
                discard:0, list_bytes:512, pages:128, block_capacity:64,
                write:32, read:0, min_pages:128, max_pages:128, id,
            }).unwrap();
        // Model the witnessed firmware lookup (TEXT2eb70/2ec90). Register
        // all owners before consuming any, as overlapped tiling can do.
        let slot = u32::from_le_bytes(manager[12..16].try_into().unwrap()) as usize;
        hardware_table[slot] = pages;
        managers.push((manager, pages));
    }
    for (manager, expected) in managers {
        let slot = u32::from_le_bytes(manager[12..16].try_into().unwrap()) as usize;
        assert_eq!(hardware_table[slot], expected, "another owner replaced the page list");
    }
}
