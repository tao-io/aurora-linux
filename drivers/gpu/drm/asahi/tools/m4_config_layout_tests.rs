// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Baseline placeholders populated only by test-config-layout.py from Git.
#[path = "g16_config_layout.rs"] mod layout;
use layout::*;
const OLD_OWNERS:&[Object]=&[/* BASELINE_OWNERS */];
const OLD_SIZES:&[usize]=&[/* BASELINE_SIZES */];
const OLD_IO:&[(usize,u64,usize,usize,u64,u64)]=&[/* BASELINE_IOMAPS */];
#[test]
fn owner_sizes_order_and_client_views_match_original() {
    assert_eq!(OBJECTS,OLD_OWNERS);
    for (i,owner) in OBJECTS.into_iter().enumerate() {
        assert_eq!(owner as usize,i);assert_eq!(owner.size(),OLD_SIZES[i]);
        assert_eq!(owner.needs_client_mapping(),matches!(owner,Object::UmaTable|Object::ParameterTable));
    }
}
#[test]
fn every_mmio_record_and_mapping_topology_matches_original() {
    assert_eq!(IOMAPS.len(),OLD_IO.len());
    for delta in [0u64,0x10000000,0x20000000] {
        let mut table=[0;IO_TABLE_SIZE];let mut old_table=table;
        for (i,io) in IOMAPS.into_iter().enumerate() {
            let (slot,physical,mapped,requested,tag,mode)=OLD_IO[i];
            assert_eq!((io.slot,io.physical.0,io.mapped,io.requested,io.tag,io.mode),OLD_IO[i]);
            let offset=physical&0x3fff;let va=0xfffffc2040000000+delta+i as u64*0x400000+offset;
            assert_eq!(io.physical.page_base(),physical-offset);assert_eq!(io.physical.page_offset(),offset as usize);
            assert_eq!(io.mapping_size().unwrap(),if slot==3 {0x18000}else{(mapped+offset as usize+0x3fff)&!0x3fff});
            assert_eq!(io.second_bank().unwrap(),if slot==3 {Some((0x18000,PhysicalAddress(physical+0x2000000),0x18000))}else{None});
            assert_eq!(io.protection(),if slot==36 {Protection::Protected}else if mode==0 {Protection::ReadOnly}else{Protection::ReadWrite});
            let entry=&mut old_table[slot*0x28..(slot+1)*0x28];
            entry[0..8].copy_from_slice(&physical.to_le_bytes());entry[8..16].copy_from_slice(&va.to_le_bytes());
            entry[16..20].copy_from_slice(&(mapped as u32).to_le_bytes());entry[20..24].copy_from_slice(&(requested as u32).to_le_bytes());
            entry[24..32].copy_from_slice(&tag.to_le_bytes());entry[32..40].copy_from_slice(&mode.to_le_bytes());
            table[slot*IO_ENTRY_SIZE..(slot+1)*IO_ENTRY_SIZE].copy_from_slice(&io.encode(FirmwareVa::new(va).unwrap()).unwrap());
        }
        assert_eq!(table,old_table);
    }
}
#[test]
fn reject_invalid_addresses_extents_slots_and_overflow() {
    assert!(FirmwareVa::new(0x300000000).is_err());
    let va=FirmwareVa::new(0xfffffc2040000000).unwrap();let mut io=IOMAPS[0];
    io.slot=IO_SLOTS;assert_eq!(io.encode(va),Err(Error::Bounds));
    io=IOMAPS[0];io.mapped=usize::MAX;assert!(io.mapping_size().is_err());assert_eq!(io.encode(va),Err(Error::Bounds));
    io=IOMAPS[0];io.requested=0;assert_eq!(io.encode(va),Err(Error::Bounds));
    io=IOMAPS[0];assert_eq!(io.encode(FirmwareVa::new(0xfffffc2040000001).unwrap()),Err(Error::Address));
    assert_eq!(io.encode(FirmwareVa::new(0xffffffffffffc000).unwrap()),Err(Error::Address));
    io=IOMAPS[1];assert_eq!(io.slot,3);io.physical=PhysicalAddress(u64::MAX);assert_eq!(io.second_bank(),Err(Error::Address));
}
