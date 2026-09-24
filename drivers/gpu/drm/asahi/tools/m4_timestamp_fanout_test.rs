// SPDX-License-Identifier: GPL-2.0-only
#![allow(dead_code)]
#[path = "../g16_render.rs"] mod g16_render;
#[path = "../g16_compute.rs"] mod g16_compute;
#[path = "../g16_render_command.rs"] mod wire;
#[test]
fn moves_finalize_preserving_restart_and_all_destinations() {
    for count in [0, 1, 8] {
        let base = 0xffff_fc20_0100_0000;
        let args = wire::Args {command:base, sku:base+0x4000, queue:base+0x8000,
            stats:base+0xc000, pb_slot:base+0x10000, manager:base+0x14000,
            pb_aux:base+0x18000, notifier:base+0x1c000, shared_stamp:base+0x20000,
            stamp_address:base+0x24000, uma:base+0x28000, uma_aux:0x7003000000,
            timestamp_storage:base+0x30000, context:3, generation:2, counter:42,
            uuid:1234, stamp:0x300, event_slot:0x71, fragment_slot:0x71,
            fragment_stamp:0x300, pb_id:0, pass_id:0};
        let mut cmd = [0; 0xca0]; let mut seq = [0; 0x800];
        wire::encode(&mut cmd, &mut seq, g16_render::Stage::Fragment, args).unwrap();
        let original = seq;
        let destinations: Vec<_> = (0..count).map(|i| base+0x38000+i*16).collect();
        let size = wire::fragment_timestamp_copies(&mut seq, &destinations).unwrap();
        assert_eq!(size, 0x380 + count as usize * 0x4c);
        assert_eq!(&seq[..0x288], &original[..0x288]);
        for (i, address) in destinations.iter().enumerate() {
            let record = &seq[0x288+i*0x4c..0x288+(i+1)*0x4c];
            assert_eq!(&record[..0x24], &original[0x23c..0x260]);
            assert_eq!(u64::from_le_bytes(record[0x24..0x2c].try_into().unwrap()), *address);
            assert_eq!(&record[0x2c..], &original[0x268..0x288]);
        }
        let finalize = 0x288 + count as usize * 0x4c;
        assert_eq!(&seq[finalize..finalize+0xa0], &original[0x288..0x328]);
        assert_eq!(i32::from_le_bytes(seq[finalize+0xa0..finalize+0xa4].try_into().unwrap()), -(finalize as i32));
        assert_eq!(&seq[finalize+0xa4..size], &original[0x32c..0x380]);
    }
}
#[test]
fn rejects_invalid_destinations_and_capacity_before_mutation() {
    let mut seq=[0xa5;0x800];let before=seq;
    assert!(wire::fragment_timestamp_copies(&mut seq, &[0]).is_err());
    assert!(wire::fragment_timestamp_copies(&mut seq, &[0xffff_fc20_0100_0000;9]).is_err());
    assert!(wire::fragment_timestamp_copies(&mut seq[..0x380], &[0xffff_fc20_0100_0000]).is_err());
    assert_eq!(seq,before);
}
