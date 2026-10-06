// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-board description of the 25G83 (RTKit-3255.160.4) GPU firmware path.
//!
//! J713 (M4, G16G) and J613 (M3, G15G) run the same firmware RTKit release and InitData root ABI,
//! but each loads its own firmware image and needs its own identity, MMIO, performance tables and
//! loader table. J713 values are the constructor this path was qualified with. J613 values come
//! from macOS 26.6.2 (25G83) on J613, captured under the m1n1 hypervisor (attempts 903-905; see
//! mac-aurora-omarchy machines/m3-air/research/2026-09-24-g15g-25g83).

use crate::g16_config::layout::{IoMap, PhysicalAddress};
use crate::hw;

/// Largest I/O-map table of any board (J613's fixed Main table holds 53 slots, up to Main+0xe88).
pub(crate) const MAX_IO_SLOTS: usize = 53;

#[derive(Debug)]
pub(crate) struct FirmwareImage {
    pub(crate) text_size: u64,
    pub(crate) data_size: u64,
    /// SHA-256 of TEXT with the GKTS and ECAP boot-argument values zeroed.
    pub(crate) text_sha256: [u8; 32],
    /// TEXT+0x22c: the boot-argument TLV table offset/size header.
    pub(crate) tlv_header: [u8; 8],
    pub(crate) gkts: usize,
    pub(crate) ecap: usize,
    /// DATA offset of the loader's host-supplied I/O-map table.
    pub(crate) loader_iomaps: usize,
}

#[derive(Debug)]
pub(crate) struct Board {
    pub(crate) name: &'static str,
    pub(crate) root_compatible: [&'static [u8]; 2],
    pub(crate) gpu_compatible: &'static [u8],
    pub(crate) asc: (u64, u64),
    pub(crate) sgx: (u64, u64),
    pub(crate) mbox_compatible: &'static [u8],
    pub(crate) mbox_reg: [u32; 4],
    pub(crate) mbox_irqs: [u32; 12],
    /// The bootloader's handoff-version property (m1n1-asahi: g16, rubra m1n1 0004: m3).
    pub(crate) handoff_version: &'static kernel::str::CStr,
    pub(crate) firmware: FirmwareImage,
    /// SGX+0xd04000 version and SGX+0xd04010 counts; SGX+0xe01500 core mask must fit `core_mask`.
    pub(crate) sgx_version: u32,
    pub(crate) sgx_counts: u32,
    pub(crate) core_mask: u32,
    /// PMP logical device of the AGX power vote (command 0xf).
    pub(crate) pmp_device: u16,
    pub(crate) io_slots: usize,
    pub(crate) iomaps: &'static [IoMap],
    /// Empty slots macOS still marks with mode 2.
    pub(crate) empty_mode2: &'static [usize],
    /// Slot mapped with PROT_FW_PROTECTED_MMIO, if any.
    pub(crate) protected_slot: Option<usize>,
    /// Fender SRAM (SGX+0xd60000) scratch published at Runtime+0x18.
    pub(crate) scratch: u64,
    pub(crate) frequencies_mhz: &'static [u32],
    pub(crate) aux_frequencies_mhz: &'static [u32],
    pub(crate) voltages_mv: &'static [u32],
    pub(crate) sram_voltages_mv: &'static [u32],
    pub(crate) main_f24: u64,
    pub(crate) cores: u32,
    pub(crate) main_2540: [(usize, u32); 7],
    /// Static Main words written after the constructor (macOS capture), if enabled.
    pub(crate) main_overlay: &'static [(usize, u32)],
    /// PowerPerformance words the firmware needs that the constructor leaves zero
    /// (J613: the MTR alarm sensor mask; zero makes every alarm fatal).
    pub(crate) power_overlay: &'static [(usize, u32)],
    /// Map Main and PowerPerformance firmware-cacheable (atomics in the firmware's MTR path).
    pub(crate) cached_hw_objects: bool,
    pub(crate) gpu_gen: hw::GpuGen,
    pub(crate) gpu_revision: hw::GpuRevision,
    pub(crate) chip_id: u32,
    /// The J713 thermal monitor reads T8132 PMP SRAM records; boards without one run uncooled.
    pub(crate) pmp_thermal: bool,
    /// The boot compute self-test runs a G16 shader; other ISAs skip it.
    pub(crate) compute_self_test: bool,
}

impl Board {
    pub(crate) fn max_pstate(&self) -> u32 { (self.frequencies_mhz.len() - 1) as u32 }
}

const fn iomap(slot: usize, physical: u64, mapped: usize, requested: usize, tag: u64, mode: u64) -> IoMap {
    IoMap { slot, physical: PhysicalAddress(physical), mapped, requested, tag, mode }
}

pub(crate) static J713: Board = Board {
    name: "J713",
    root_compatible: [b"apple,j713", b"apple,t8132"],
    gpu_compatible: b"apple,agx-t8132\0",
    asc: (0x302600000, 0x4000),
    sgx: (0x300000000, 0x4000000),
    mbox_compatible: b"apple,t8132-asc-mailbox\0apple,asc-mailbox-v4\0",
    mbox_reg: [3, 0x02608000, 0, 0x4000],
    mbox_irqs: [0, 993, 4, 0, 994, 4, 0, 995, 4, 0, 996, 4],
    handoff_version: kernel::c_str!("apple,g16-handoff-version"),
    firmware: FirmwareImage {
        text_size: 0x60000,
        data_size: 0x13c000,
        text_sha256: [
            0x5f, 0x65, 0xf6, 0x14, 0x58, 0x72, 0x54, 0x70, 0xb4, 0xb0, 0xc0, 0x20, 0x28, 0x47, 0x7b, 0xf2,
            0xea, 0x57, 0x3a, 0xfb, 0x4e, 0x0c, 0x49, 0x6e, 0xf9, 0x95, 0xe9, 0x6a, 0xff, 0xb0, 0xd8, 0x6c,
        ],
        tlv_header: [0xdf, 0xc8, 5, 0, 0x31, 2, 0, 0],
        gkts: 0x5c8df,
        ecap: 0x5c9c2,
        loader_iomaps: 0x1332f0,
    },
    sgx_version: 0x0a021100,
    sgx_counts: 0x0011010a,
    core_mask: 0x3ff,
    pmp_device: 5,
    io_slots: 42,
    iomaps: &[
        iomap(0, 0x381014000, 0x4000, 0x4000, 0, 2),
        iomap(3, 0x2201c4000, 0x30000, 0x18000, 0, 2),
        iomap(9, 0x3803d0000, 0x1000, 0x1000, 0, 2),
        iomap(10, 0x3803c0000, 0x2000, 0x2000, 0, 0),
        iomap(12, 0x50165c000, 0x4000, 0x4000, 0, 2),
        iomap(14, 0x380280000, 0x8000, 0x8000, 0, 0),
        iomap(15, 0x38c840000, 0x24000, 0x24000, 0, 0),
        iomap(17, 0x300000000, 0x20000, 0x20000, 0, 2),
        iomap(22, 0x301000000, 0x8000, 0x8000, 0, 2),
        iomap(26, 0x300d04000, 0x8000, 0x8000, 0xd04000, 2),
        iomap(27, 0x300d0d000, 0x1000, 0x1000, 0xd0d000, 2),
        iomap(28, 0x300d58000, 0x8000, 0x8000, 0xd58000, 2),
        iomap(29, 0x300d10000, 0x4000, 0x4000, 0xd10000, 2),
        iomap(31, 0x300d40000, 0x4000, 0x4000, 0xd40000, 2),
        iomap(32, 0x300d60000, 0x8000, 0x8000, 0xd60000, 2),
        iomap(36, 0x300e00000, 0x4000, 0x4000, 0xe00000, 0),
        iomap(39, 0x300e08000, 0x8000, 0x8000, 0, 2),
        iomap(40, 0x300e1c000, 0x4000, 0x4000, 0xe1c000, 2),
        iomap(41, 0x300e1f800, 0x4000, 0x4000, 0, 2),
    ],
    empty_mode2: &[],
    protected_slot: Some(36),
    scratch: 0x300d60000,
    frequencies_mhz: &[0, 338, 618, 796, 928, 1056, 1170, 1278, 1338, 1470],
    aux_frequencies_mhz: &[0, 338, 618, 796, 836, 952, 1053, 1152, 1204, 1326],
    voltages_mv: &[125, 610, 645, 680, 725, 780, 825, 865, 895, 970],
    sram_voltages_mv: &[780, 780, 780, 780, 780, 780, 825, 865, 895, 970],
    main_f24: 0x0000000607002004,
    cores: 10,
    main_2540: [(4, 4), (8, 6), (12, 3), (16, 7), (20, 7), (40, 8), (48, 1)],
    main_overlay: &[],
    power_overlay: &[],
    cached_hw_objects: false,
    gpu_gen: hw::GpuGen::G16,
    gpu_revision: hw::GpuRevision::B0,
    chip_id: 0x8132,
    pmp_thermal: true,
    compute_self_test: true,
};

// Generated from HV attempt 903 (macOS 26.6.2 25G83 on J613, Main object = HwData +0).
const J613_IOMAPS: &[IoMap] = &[
    IoMap { slot: 0, physical: PhysicalAddress(0x2d1014000), mapped: 0x4000, requested: 0x4000, tag: 0x0, mode: 2 },
    IoMap { slot: 3, physical: PhysicalAddress(0x2201c4000), mapped: 0x30000, requested: 0x18000, tag: 0x0, mode: 2 },
    IoMap { slot: 9, physical: PhysicalAddress(0x2d03d0000), mapped: 0x1000, requested: 0x1000, tag: 0x0, mode: 2 },
    IoMap { slot: 10, physical: PhysicalAddress(0x2d03c0000), mapped: 0x2000, requested: 0x2000, tag: 0x0, mode: 0 },
    IoMap { slot: 12, physical: PhysicalAddress(0x31145c000), mapped: 0x4000, requested: 0x4000, tag: 0x0, mode: 2 },
    IoMap { slot: 14, physical: PhysicalAddress(0x2d0280000), mapped: 0x8000, requested: 0x8000, tag: 0x0, mode: 0 },
    IoMap { slot: 17, physical: PhysicalAddress(0x290000000), mapped: 0x20000, requested: 0x20000, tag: 0x0, mode: 2 },
    IoMap { slot: 26, physical: PhysicalAddress(0x290d04000), mapped: 0x8000, requested: 0x8000, tag: 0xd04000, mode: 2 },
    IoMap { slot: 27, physical: PhysicalAddress(0x290d0d000), mapped: 0x1000, requested: 0x1000, tag: 0xd0d000, mode: 2 },
    IoMap { slot: 28, physical: PhysicalAddress(0x290d50000), mapped: 0x8000, requested: 0x8000, tag: 0xd50000, mode: 2 },
    IoMap { slot: 29, physical: PhysicalAddress(0x290d10000), mapped: 0x4000, requested: 0x4000, tag: 0xd10000, mode: 2 },
    IoMap { slot: 31, physical: PhysicalAddress(0x290d40000), mapped: 0x4000, requested: 0x4000, tag: 0xd40000, mode: 2 },
    IoMap { slot: 32, physical: PhysicalAddress(0x290d60000), mapped: 0x8000, requested: 0x8000, tag: 0xd60000, mode: 2 },
    IoMap { slot: 35, physical: PhysicalAddress(0x290e00000), mapped: 0x4000, requested: 0x4000, tag: 0xe00000, mode: 2 },
    IoMap { slot: 39, physical: PhysicalAddress(0x290e08000), mapped: 0x8000, requested: 0x8000, tag: 0x0, mode: 2 },
    IoMap { slot: 40, physical: PhysicalAddress(0x290e1c000), mapped: 0x4000, requested: 0x4000, tag: 0xe1c000, mode: 2 },
];
// Main+0xf38 selects asynchronous pstate completion. Leave zero to exercise
// the firmware's bounded hardware-status polling path (also used by J713).
// macOS uses IRQ completion; Linux g7/g8 parked waiting on its event.
/// Static Main capture overlay, including the g8 prefix A/B words.
const J613_MAIN_OVERLAY: &[(usize, u32)] = &[
    // g8 A/B: 33 stable non-pointer Main words absent in g7 (HV 903/905).
    (0x4, 0x6f),
    (0x8, 0xffc00000),
    (0x14, 0x10),
    (0x1c, 0x10),
    (0x20, 0xffff8000),
    (0x24, 0x2ff),
    (0xe0, 0x2008),
    (0xe8, 0x20080000),
    (0xf4, 0x2008),
    (0xf8, 0x24cb),
    (0xfc, 0xc98a2cfa),
    (0x100, 0xf4f624cb),
    (0x104, 0x1877e917),
    (0x108, 0x38d924cb),
    (0x10c, 0xbdab0000),
    (0x2d8, 0xfff82000),
    (0x2dc, 0xd32d2cdb),
    (0x2e0, 0xf5002000),
    (0x2e4, 0x21dae926),
    (0x2e8, 0x38b62000),
    (0x2ec, 0xc7420008),
    (0x3e0, 0x7fe0),
    (0x3e8, 0x7fe00000),
    (0x3f4, 0x7fe0),
    (0x3f8, 0x8000),
    (0x400, 0x80000000),
    (0x40c, 0x8000),
    (0x5d8, 0x4b232645),
    (0x5dc, 0xe98),
    (0x5e0, 0xd5a2ea5f),
    (0x5e4, 0x40003fff),
    (0x5e8, 0xca5e4000),
    (0x5ec, 0x4000f5a2),
    (0xe90, 0x8122), (0xe94, 0x2), (0xe9c, 0x4), (0xea4, 0x1), (0xec0, 0x1), (0xec8, 0x1),
    (0xee4, 0x1), (0xee8, 0x1), (0xf04, 0x1f), (0xf34, 0x1), (0xf38, 0x0), (0xf4c, 0x31),
    (0xf6c, 0x1), (0xf88, 0xa), (0xf8c, 0x3), (0xfac, 0x1), (0xfb8, 0x16), (0xfbc, 0x5),
    (0x1848, 0x3f828f5c), (0x184c, 0x3f828f5c), (0x1850, 0x3f828f5c), (0x1854, 0x3f828f5c), (0x1858, 0x3f828f5c), (0x185c, 0x3f828f5c),
    (0x1860, 0x3f828f5c), (0x1864, 0x3f828f5c), (0x1868, 0x3f828f5c), (0x18cc, 0xf), (0x18d0, 0x19), (0x18d4, 0x24),
    (0x18d8, 0x2e), (0x18dc, 0x3a), (0x18e0, 0x4b), (0x18e4, 0x5a), (0x18e8, 0x64), (0x1910, 0x1c),
    (0x1914, 0x2d), (0x1918, 0x3b), (0x191c, 0x47), (0x1920, 0x53), (0x1924, 0x5e), (0x1928, 0x64),
    (0x19cc, 0x1), (0x19d0, 0x2), (0x19d4, 0x3), (0x19d8, 0x5), (0x19dc, 0x7), (0x19e0, 0x9),
    (0x19e4, 0xb), (0x19e8, 0xd), (0x1a0c, 0x1), (0x1a10, 0x2), (0x1a14, 0x3), (0x1a18, 0x4),
    (0x1a1c, 0x6), (0x1a20, 0x8), (0x1a24, 0xa), (0x1a28, 0xc), (0x1cd8, 0x8), (0x1ce0, 0x152),
    (0x1ce4, 0x26a), (0x1ce8, 0x31c), (0x1cec, 0x344), (0x1cf0, 0x3b8), (0x1cf4, 0x41d), (0x1cf8, 0x480),
    (0x1cfc, 0x4b4), (0x1d5c, 0x27b), (0x1d9c, 0x29e), (0x1ddc, 0x2cb), (0x1e1c, 0x2f3), (0x1e5c, 0x320),
    (0x1e9c, 0x35c), (0x1edc, 0x384), (0x1f1c, 0x39d), (0x211c, 0x316), (0x215c, 0x316), (0x219c, 0x316),
    (0x21dc, 0x316), (0x221c, 0x316), (0x225c, 0x320), (0x229c, 0x35c), (0x22dc, 0x384), (0x231c, 0x39d),
    (0x258c, 0x160000), (0x2594, 0x1), (0x2598, 0x1), (0x25a4, 0x1), (0x25a8, 0x1), (0x25ac, 0x1),
    (0x25b4, 0xffffffff), (0x25b8, 0xffffffff), (0x25bc, 0xffffffff), (0x25c0, 0xffffffff), (0x25c4, 0xffffffff), (0x25c8, 0xffffffff),
    (0x25cc, 0xffffffff), (0x25d0, 0xffffffff), (0x25d4, 0xffffffff), (0x25d8, 0xffffffff), (0x25dc, 0xffffffff), (0x25e0, 0xffffffff),
    (0x2604, 0x1), (0x2634, 0x1),
];

// macOS 26 J613 PowerPerformance thermal config (HV 903): temperature controller +0x21f0..+0x22c8,
// SoCHot +0x2e2c..+0x2f14 (sensor mask 0x4248, alarm threshold 125 C), MTR mask/enable +0x3b58/+0x3b60. 38 words.
const J613_POWER_OVERLAY: &[(usize, u32)] = &[
    (0x21f0, 0x47800000), (0x21f4, 0x41f00000), (0x21f8, 0x44480000), (0x21fc, 0x53a), (0x2208, 0x10000), (0x2210, 0x320),
    (0x2218, 0x42c80000), (0x221c, 0x320), (0x2224, 0x3f4ccccd), (0x2228, 0x3e4ccccd), (0x2264, 0x1469), (0x2268, 0x4248),
    (0x2270, 0x1f40), (0x2278, 0x4), (0x228c, 0x3f800000), (0x2294, 0x406fffff), (0x22a0, 0x47800000), (0x22a4, 0x42080000),
    (0x22b0, 0x1e), (0x22b4, 0x320), (0x22b8, 0x320), (0x22c4, 0x2af8), (0x22c8, 0x1469), (0x2e2c, 0x4),
    (0x2e38, 0x3f780000), (0x2e40, 0x3d000000), (0x2e48, 0x40a00000), (0x2e54, 0x47800000), (0x2e58, 0x41700000), (0x2e64, 0x2bc),
    (0x2e68, 0x320), (0x2e6c, 0x320), (0x2e78, 0x320), (0x2f08, 0x4248), (0x2f10, 0x7d), (0x2f14, 0x1),
    (0x3b58, 0x4b48), (0x3b60, 0x1),
];

pub(crate) static J613: Board = Board {
    name: "J613",
    root_compatible: [b"apple,j613", b"apple,t8122"],
    gpu_compatible: b"apple,agx-t8122\0",
    asc: (0x292400000, 0x4000),
    sgx: (0x290000000, 0x4000000),
    mbox_compatible: b"apple,t8122-asc-mailbox\0apple,asc-mailbox-v4\0",
    mbox_reg: [2, 0x92408000, 0, 0x4000],
    mbox_irqs: [0, 723, 4, 0, 724, 4, 0, 725, 4, 0, 726, 4],
    handoff_version: kernel::c_str!("apple,m3-handoff-version"),
    firmware: FirmwareImage {
        text_size: 0x60000,
        data_size: 0x12c000,
        // IPSW c000 image as loaded by 25G83 iBoot; identical on two boots (HV 904, 905).
        text_sha256: [
            0x3a, 0x6b, 0x8b, 0x5e, 0x83, 0x38, 0x4d, 0x5c, 0xc1, 0x92, 0x9d, 0x91, 0xb6, 0x36, 0x40, 0xfe,
            0xe8, 0xf5, 0x52, 0xb5, 0x10, 0x8c, 0x4e, 0xa6, 0x20, 0xbe, 0xdb, 0x7b, 0xbc, 0xa0, 0x31, 0x64,
        ],
        tlv_header: [0x4c, 0xce, 5, 0, 0x31, 2, 0, 0],
        gkts: 0x5ce4c,
        ecap: 0x5cf2f,
        // c000's descriptor copy target; equal to Main+0x640 at runtime (HV 905).
        loader_iomaps: 0x1228e0,
    },
    sgx_version: 0x07022000,
    sgx_counts: 0x0011010a,
    core_mask: 0x3ff,
    pmp_device: 5,
    io_slots: MAX_IO_SLOTS,
    iomaps: J613_IOMAPS,
    empty_mode2: &[2, 5, 6, 7, 8, 30, 33, 34, 37, 38, 41, 42, 43, 46, 47, 48, 49, 52],
    protected_slot: None,
    scratch: 0x290d60000,
    frequencies_mhz: &[0, 338, 618, 796, 928, 1056, 1170, 1278, 1338],
    aux_frequencies_mhz: &[0, 338, 618, 796, 836, 952, 1053, 1152, 1204],
    voltages_mv: &[125, 635, 670, 715, 755, 800, 860, 900, 925],
    sram_voltages_mv: &[790, 790, 790, 790, 790, 800, 860, 900, 925],
    main_f24: 0x4,
    cores: 10,
    main_2540: [(4, 4), (8, 6), (12, 3), (16, 7), (20, 7), (40, 10), (48, 1)],
    main_overlay: J613_MAIN_OVERLAY,
    power_overlay: J613_POWER_OVERLAY,
    cached_hw_objects: true,
    gpu_gen: hw::GpuGen::G15,
    gpu_revision: hw::GpuRevision::C0,
    chip_id: 0x8122,
    pmp_thermal: false,
    compute_self_test: false,
};

static CURRENT: core::sync::atomic::AtomicPtr<Board> = core::sync::atomic::AtomicPtr::new(core::ptr::null_mut());

/// The detected board (cached after the first successful detection).
#[cfg(not(test))]
pub(crate) fn get() -> kernel::error::Result<&'static Board> {
    use core::sync::atomic::Ordering;
    let cached = CURRENT.load(Ordering::Acquire);
    if !cached.is_null() {
        // SAFETY: only ever set to a pointer to one of the statics above.
        return Ok(unsafe { &*cached });
    }
    let board = current().ok_or(kernel::error::code::ENODEV)?;
    CURRENT.store(board as *const Board as *mut Board, Ordering::Release);
    Ok(board)
}

/// The board this kernel is running on, by the root compatible; None for other machines.
#[cfg(not(test))]
pub(crate) fn current() -> Option<&'static Board> {
    use kernel::{c_str, prelude::*};
    let root = kernel::of::root()?;
    let board: KVec<u8> = root.get_property(c_str!("compatible")).ok()?;
    [&J713, &J613].into_iter().find(|b| {
        b.root_compatible.iter().all(|want| board.split(|c| *c == 0).any(|s| s == *want))
    })
}
