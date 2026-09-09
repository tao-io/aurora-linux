// SPDX-License-Identifier: GPL-2.0-only
// rustc --edition=2021 --test tests/g16_render_dependency.rs -o /tmp/g16-render-dependency
#![allow(dead_code)]
#[path = "../g16_render.rs"]
mod g16_render;
#[path = "../g16_render_command.rs"]
mod g16_render_command;

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
