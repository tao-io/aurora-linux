// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! J713/25G83 configuration allocation and IOMapping contracts. Preserve the
//! qualified producer e8a21feb3c47, including native slot/mode/tag values.
//! These are board-specific descriptions; empty firmware table slots stay zero.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
#[repr(usize)]
pub(crate) enum Object {
    Root, Brn, Runtime, Globals, Control, FirmwareData, Power, Dynamic, Main,
    PowerPerformance, Timer, Activity254, Activity25c, Activity264, Activity26c,
    LogState, LogEntries, LogData, Fwctl, DevctrlState, DevctrlRing,
    KtraceState, KtraceData, StatisticsState, StatisticsData, Temperature,
    EventState, EventData, Retention, UmaTable, ParameterTable,
}
pub(crate) const OBJECTS:[Object;31]=[
    Object::Root,Object::Brn,Object::Runtime,Object::Globals,Object::Control,Object::FirmwareData,Object::Power,Object::Dynamic,Object::Main,Object::PowerPerformance,Object::Timer,Object::Activity254,Object::Activity25c,Object::Activity264,Object::Activity26c,Object::LogState,Object::LogEntries,Object::LogData,Object::Fwctl,Object::DevctrlState,Object::DevctrlRing,Object::KtraceState,Object::KtraceData,Object::StatisticsState,Object::StatisticsData,Object::Temperature,Object::EventState,Object::EventData,Object::Retention,Object::UmaTable,Object::ParameterTable
];
impl Object {
    pub(crate) const fn size(self)->usize {match self {
        Self::Root=>0xc8,
        Self::Brn=>0x4000,
        Self::Runtime=>0x4b8,
        Self::Globals=>0xe48,
        Self::Control=>0x20,
        Self::FirmwareData=>0xeaf0,
        Self::Power=>0x670,
        Self::Dynamic=>0x4000,
        Self::Main=>0x2658,
        Self::PowerPerformance=>0x6578,
        Self::Timer=>0x88,
        Self::Activity254=>0xc18,
        Self::Activity25c=>0x1248,
        Self::Activity264=>0xe10,
        Self::Activity26c=>0x60,
        Self::LogState=>0x1b0,
        Self::LogEntries=>0x28800,
        Self::LogData=>0x79800,
        Self::Fwctl=>0x4000,
        Self::DevctrlState=>0x30,
        Self::DevctrlRing=>0x4000,
        Self::KtraceState=>0x30,
        Self::KtraceData=>0x9000,
        Self::StatisticsState=>0x30,
        Self::StatisticsData=>0x4800,
        Self::Temperature=>0x4,
        Self::EventState=>0x30,
        Self::EventData=>0x4800,
        Self::Retention=>0x4000,
        Self::UmaTable=>0x4000,
        Self::ParameterTable=>0x4000,
    }}
    pub(crate) const fn needs_client_mapping(self)->bool {
        matches!(self,Self::UmaTable|Self::ParameterTable)
    }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) enum Error {Address,Bounds}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) enum Protection {ReadOnly,ReadWrite,Protected}
/// Physical register address, intentionally distinct from its owned firmware VA.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) struct PhysicalAddress(pub(crate) u64);
impl PhysicalAddress {
    pub(crate) const fn page_offset(self)->usize {(self.0&0x3fff) as usize}
    pub(crate) const fn page_base(self)->u64 {self.0&!0x3fff}
}
/// The mapped device range has already been allocated by UAT. Preserve its
/// subpage offset instead of accidentally publishing the aligned base.
#[derive(Clone,Copy,Debug)]
pub(crate) struct FirmwareVa(u64);
impl FirmwareVa {
    pub(crate) fn new(value:u64)->Result<Self,Error> {
        if value<0xffff_fc00_0000_0000 {return Err(Error::Address);}
        Ok(Self(value))
    }
}
pub(crate) const IO_SLOTS:usize=42;
pub(crate) const IO_ENTRY_SIZE:usize=0x28;
pub(crate) const IO_TABLE_SIZE:usize=IO_SLOTS*IO_ENTRY_SIZE;
#[derive(Clone,Copy,Debug)]
pub(crate) struct IoMap {
    pub(crate) slot:usize,pub(crate) physical:PhysicalAddress,
    pub(crate) mapped:usize,pub(crate) requested:usize,
    pub(crate) tag:u64,pub(crate) mode:u64,
}
impl IoMap {
    pub(crate) fn protection(self)->Protection {
        if self.slot==36 {Protection::Protected}
        else if self.mode==0 {Protection::ReadOnly}else{Protection::ReadWrite}
    }
    pub(crate) fn mapping_size(self)->Result<usize,Error> {
        // MCC slot 3 concatenates two 96-KiB banks 32 MiB apart. Preserve the
        // existing reservation/mapping sequence; a descriptor is not proof
        // that the corresponding physical range is contiguous.
        if self.slot==3 {return Ok(0x18000);}
        self.mapped.checked_add(self.physical.page_offset()).and_then(|n|n.checked_add(0x3fff))
            .map(|n|n&!0x3fff).ok_or(Error::Bounds)
    }
    pub(crate) fn second_bank(self)->Result<Option<(u64,PhysicalAddress,usize)>,Error> {
        if self.slot!=3 {return Ok(None);}
        Ok(Some((0x18000,PhysicalAddress(self.physical.0.checked_add(0x2000000).ok_or(Error::Address)?),0x18000)))
    }
    /// Checked byte encoding; no packed Rust references and no host padding.
    /// Returns a complete new record, so rejection cannot mutate caller bytes.
    pub(crate) fn encode(self,va:FirmwareVa)->Result<[u8;IO_ENTRY_SIZE],Error> {
        if self.slot>=IO_SLOTS || self.mapped==0 || self.requested==0
            || self.mapped>u32::MAX as usize || self.requested>u32::MAX as usize {
            return Err(Error::Bounds);
        }
        if va.0&0x3fff!=self.physical.0&0x3fff || va.0.checked_add(self.mapped as u64).is_none() {
            return Err(Error::Address);
        }
        let mut out=[0;IO_ENTRY_SIZE];
        out[0..8].copy_from_slice(&self.physical.0.to_le_bytes());
        out[8..16].copy_from_slice(&va.0.to_le_bytes());
        out[16..20].copy_from_slice(&(self.mapped as u32).to_le_bytes());
        out[20..24].copy_from_slice(&(self.requested as u32).to_le_bytes());
        out[24..32].copy_from_slice(&self.tag.to_le_bytes());
        out[32..40].copy_from_slice(&self.mode.to_le_bytes());Ok(out)
    }
}
pub(crate) const IOMAPS:[IoMap;19]=[
    IoMap {slot:0,physical:PhysicalAddress(0x381014000),mapped:0x4000,requested:0x4000,tag:0x0,mode:2},
    IoMap {slot:3,physical:PhysicalAddress(0x2201c4000),mapped:0x30000,requested:0x18000,tag:0x0,mode:2},
    IoMap {slot:9,physical:PhysicalAddress(0x3803d0000),mapped:0x1000,requested:0x1000,tag:0x0,mode:2},
    IoMap {slot:10,physical:PhysicalAddress(0x3803c0000),mapped:0x2000,requested:0x2000,tag:0x0,mode:0},
    IoMap {slot:12,physical:PhysicalAddress(0x50165c000),mapped:0x4000,requested:0x4000,tag:0x0,mode:2},
    IoMap {slot:14,physical:PhysicalAddress(0x380280000),mapped:0x8000,requested:0x8000,tag:0x0,mode:0},
    IoMap {slot:15,physical:PhysicalAddress(0x38c840000),mapped:0x24000,requested:0x24000,tag:0x0,mode:0},
    IoMap {slot:17,physical:PhysicalAddress(0x300000000),mapped:0x20000,requested:0x20000,tag:0x0,mode:2},
    IoMap {slot:22,physical:PhysicalAddress(0x301000000),mapped:0x8000,requested:0x8000,tag:0x0,mode:2},
    IoMap {slot:26,physical:PhysicalAddress(0x300d04000),mapped:0x8000,requested:0x8000,tag:0xd04000,mode:2},
    IoMap {slot:27,physical:PhysicalAddress(0x300d0d000),mapped:0x1000,requested:0x1000,tag:0xd0d000,mode:2},
    IoMap {slot:28,physical:PhysicalAddress(0x300d58000),mapped:0x8000,requested:0x8000,tag:0xd58000,mode:2},
    IoMap {slot:29,physical:PhysicalAddress(0x300d10000),mapped:0x4000,requested:0x4000,tag:0xd10000,mode:2},
    IoMap {slot:31,physical:PhysicalAddress(0x300d40000),mapped:0x4000,requested:0x4000,tag:0xd40000,mode:2},
    IoMap {slot:32,physical:PhysicalAddress(0x300d60000),mapped:0x8000,requested:0x8000,tag:0xd60000,mode:2},
    IoMap {slot:36,physical:PhysicalAddress(0x300e00000),mapped:0x4000,requested:0x4000,tag:0xe00000,mode:0},
    IoMap {slot:39,physical:PhysicalAddress(0x300e08000),mapped:0x8000,requested:0x8000,tag:0x0,mode:2},
    IoMap {slot:40,physical:PhysicalAddress(0x300e1c000),mapped:0x4000,requested:0x4000,tag:0xe1c000,mode:2},
    IoMap {slot:41,physical:PhysicalAddress(0x300e1f800),mapped:0x4000,requested:0x4000,tag:0x0,mode:2},
];
