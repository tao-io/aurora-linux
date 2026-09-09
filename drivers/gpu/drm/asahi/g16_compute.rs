// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! 25G83 M4 compute wire encoding. Addresses are supplied by owned mappings.
//! Register list and firmware microsequence for one bounded compute dispatch.

#[derive(Clone, Copy)]
pub(crate) struct Addresses {
    pub command: u64,
    pub command_gpu: u64,
    pub cdm_gpu: u64,
    pub preemption_gpu: u64,
    pub metrics: u64,
    pub auxiliary_gpu: u64,
    pub slot_state: u64,
    pub context_store_gpu: u64,
    pub pool_state_gpu: u64,
    pub page_list_gpu: u64,
    pub counter: u64,
    pub context: u32,
    pub generation: u8,
}

pub(crate) const SIZE: usize = 0x1000;
pub(crate) const SLOT: u8 = 15;
pub(crate) const STAMP: u32 = 0x100;

/// Userspace supplies a complete CDM byte range; its terminating word is
/// exclusive-end minus four. USC addresses remain in the client's 4 GiB window.
#[derive(Clone, Copy)]
pub(crate) struct Notification {
    pub address: u64,
    pub threshold: u64,
}

#[derive(Clone, Copy)]
pub(crate) struct Control {
    pub base: u64,
    pub end: u64,
    pub usc_base: u64,
}

/// Encode an owned-context dispatch with initialized USC private memory.
/// The backing owner provides at least 0x20000 bytes of context storage,
/// a pool-state array of `pool_qwords`, and a page-list allocation. The
/// qualification shader does not request spills; no captured pool is copied.
pub(crate) fn encode(out: &mut [u8], a: Addresses, pool_qwords: u32, pages: u32) -> Result<(), ()> {
    encode_dispatch(out, a, pool_qwords, pages,
        Control { base: a.cdm_gpu, end: a.cdm_gpu + 0x30, usc_base: 0 }, None)
}

pub(crate) fn encode_dispatch(out: &mut [u8], a: Addresses, pool_qwords: u32,
    pages: u32, control: Control, notification: Option<Notification>) -> Result<(), ()> {
    if control.base == 0 || control.base & 3 != 0 || control.end & 3 != 0
        || control.end <= control.base || control.end >= (1 << 42)
        || control.usc_base & 0xffff_ffff != 0 || control.usc_base >= (1 << 42)
        || out.len() < SIZE || a.command < 0xffff_fc00_0000_0000
        || a.command.checked_add(SIZE as u64).is_none()
        || a.context >= 64 || (a.context != 0 && a.generation == 0)
        || a.command & 0x3fff != 0 || a.slot_state & 7 != 0
        || a.counter & 7 != 0 || pool_qwords == 0
        || pages > pool_qwords || pages % 512 != 0
        || [a.command_gpu, a.auxiliary_gpu, a.preemption_gpu, a.context_store_gpu, a.pool_state_gpu, a.page_list_gpu]
            .iter().any(|va| *va == 0 || *va & 0x3fff != 0 || *va >= (1 << 42) - 0x20000)
        || a.metrics < 0xffff_fc00_0000_0000 || a.metrics & 7 != 0
        || a.slot_state < 0xffff_fc00_0000_0000 || a.counter < 0xffff_fc00_0000_0000 {
        return Err(());
    }
    out[..SIZE].fill(0);
    let c = a.command;
    let g = a.command_gpu;
    let sku = c + 0x900;
    let threshold = notification.map_or(c + 0xf00, |n| n.threshold);
    let uma = c + 0xe00;
    let u32_at = |b: &mut [u8], off, value: u32| b[off..off + 4].copy_from_slice(&value.to_le_bytes());
    let u64_at = |b: &mut [u8], off, value: u64| b[off..off + 8].copy_from_slice(&value.to_le_bytes());
    u32_at(out, 0, 3);
    u64_at(out, 0x10, notification.map_or(c + 0xc00, |n| n.address));
    u32_at(out, 0xc, a.context);
    out[0x88b] = a.generation;
    let registers: &[(u32, u64)] = &[
        (0x1a510, a.preemption_gpu), (0x1a420, control.base),
        (0x1a4d0, a.preemption_gpu + 0x1480), (0x1a4d8, a.preemption_gpu + 0x1488),
        (0x1a4e0, a.preemption_gpu + 0x1490), (0x1a4e8, a.preemption_gpu + 0x1498),
        (0x1a440, 0x154024201), (0x1a458, 0x10c08860),
        (0x101d9, 0x1c), (0x1a089, 0), (0x1a091, 0),
        (0x1a059, 0), (0x1a061, 0), (0x1a0b9, 0), (0x1a0c1, 0),
        (0x101d1, 0), (0xd479, 0), (0x1a0e9, 8),
        (0x107a1, 0xff0000), (0xa599, 0x13200400020),
        (0xd411, 0x200000001), (0x1a540, 1), (0x014a9, 1), (0x0a351, 1),
    ];
    for (i, (register, value)) in registers.iter().enumerate() {
        u32_at(out, 0x20 + i * 12, *register);
        u64_at(out, 0x24 + i * 12, *value);
    }
    let mut count = registers.len() as u32;
    if control.usc_base != 0 {
        u32_at(out, 0x20 + count as usize * 12, 0x10071);
        u64_at(out, 0x24 + count as usize * 12, control.usc_base);
        count += 1;
    }
    u64_at(out, 0x720, g + 0x20);
    u32_at(out, 0x728, count | ((count * 12) << 16));
    u64_at(out, 0x760, sku);
    u32_at(out, 0x768, 0x300);
    u64_at(out, 0x798, a.preemption_gpu);
    u64_at(out, 0x7a0, control.end - 4);
    u64_at(out, 0x7c8, 0x154024201);
    u32_at(out, 0x7e8, u32::MAX);
    u64_at(out, 0x800, c + 0xee4);
    u64_at(out, 0x808, c + 0xee0);
    u32_at(out, 0x810, STAMP);
    u32_at(out, 0x814, SLOT as u32);
    u32_at(out, 0x820, 1);
    u64_at(out, 0x834, c + 0x8c0);
    u64_at(out, 0x83c, c + 0x8c8);
    u64_at(out, 0x86a, uma);
    out[0x872] = 1;
    u64_at(out, 0x873, 0x1588000);
    u64_at(out, 0x87b, 0xef8000);
    u64_at(out, 0x883, a.metrics);
    // SKU start, UMA binding and context-store tuple.
    u32_at(out, 0x900, 0xb);
    for (offset, value) in [(0x14, c + 0x20), (0x1c, threshold), (0x24, a.slot_state),
        (0x44, c + 0x76c), (0x160, uma), (0x1a8, c + 0x8ac)] {
        u64_at(out, 0x900 + offset, value);
    }
    u32_at(out, 0x92c, a.context);
    u32_at(out, 0x930, a.generation as u32);
    u32_at(out, 0x950, 1);
    u64_at(out, 0xa78, a.auxiliary_gpu);
    for (i, delta) in [0, 0xf400, 0x1e800, 0x1f000].iter().enumerate() {
        u64_at(out, 0xa88 + i * 8, a.context_store_gpu + delta);
    }
    u32_at(out, 0xab8, SLOT as u32);
    for (base, opcode, selected) in [(0xabc, 0x80000003, 0x834), (0xb0c, 3, 0x83c)] {
        u32_at(out, base, opcode);
        for (off, value) in [(4, c + 0x82c), (12, c + 0x834), (20, c + selected),
            (28, a.slot_state), (44, c + 0x89c), (52, c + 0x7dc), (68, 1)] {
            u64_at(out, base + off, value);
        }
    }
    u32_at(out, 0xb08, 1); // compute WFI
    u32_at(out, 0xb58, 0xc);
    for (off, value) in [(4, threshold), (12, a.slot_state), (24, c + 0x76c),
        (36, 1), (44, c + 0xee0), (0x5c, sku + 0x160),
        (0x69, c + 0x8ac), (0x71, c + 0x88c)] {
        u64_at(out, 0xb58 + off, value);
    }
    u32_at(out, 0xb6c, a.context);
    u32_at(out, 0xb8c, STAMP);
    u32_at(out, 0xbbc, (-0x258i32) as u32);
    u32_at(out, 0xbd4, 0x40000002);
    // Independently consumed notifier and retirement stamps.
    u64_at(out, 0xc00, threshold);
    u32_at(out, 0xc08, 1);
    u32_at(out, 0xc10, 0x50);
    out[0xca8..0xcb0].fill(255);
    u32_at(out, 0xd14, 1);
    u32_at(out, 0xee0, u32::MAX);
    u32_at(out, 0xee4, u32::MAX);
    u64_at(out, 0xf00, 1);
    // Firmware-owned descriptor; its counter stays canonical for CPU access.
    u64_at(out, 0xe00, 1);
    for (off, value) in [(8, 1), (12, u32::from(pages == 0)), (16, 2), (0x1c, pool_qwords),
        (0x24, pages), (0x28, u32::from(pages == 0)), (0x2c, pages),
        (0x40, 4), (0x48, pages / 512 * 8), (0x5c, u32::from(pages != 0)),
        (0x84, u32::from(pages != 0)), (0x94, 2)] {
        u32_at(out, 0xe00 + off, value);
    }
    u64_at(out, 0xe14, a.pool_state_gpu);
    u64_at(out, 0xe30, a.page_list_gpu);
    u64_at(out, 0xe4c, a.counter);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn addresses() -> Addresses {
        Addresses { command: 0xfffffc2000014000, command_gpu: 0x7000100000, cdm_gpu: 0x7000500000, preemption_gpu: 0x7000600000, metrics: 0xfffffc2000050000, auxiliary_gpu: 0x7000700000,
            slot_state: 0xfffffc2000030000, context_store_gpu: 0x7000800000,
            pool_state_gpu: 0x7000200000, page_list_gpu: 0x7000300000,
            counter: 0xfffffc2000040000, context: 1, generation: 1 }
    }
    #[test]
    fn packed_abi_roles_and_end_record() {
        let mut bytes = [0; SIZE];
        encode(&mut bytes, addresses(), 16384, 512).unwrap();
        let u64_at = |off| u64::from_le_bytes(bytes[off..off + 8].try_into().unwrap());
        assert_eq!(&bytes[0xc..0x10], &[1, 0, 0, 0]);
        assert_eq!(bytes[0x88b], 1);
        assert_eq!(&bytes[0x92c..0x934], &[1, 0, 0, 0, 1, 0, 0, 0]);
        assert_eq!(u64_at(0x7a0), 0x700050002c);
        assert_eq!(u64_at(0x30), 0x7000500000);
        assert_eq!(u64_at(0x720), 0x7000100020);
        assert_eq!(u64_at(0x914), 0xfffffc2000014020);
        assert_eq!(&bytes[0x728..0x72c], &[24, 0, 0x20, 1]);
        assert_eq!(u64_at(0x86a), 0xfffffc2000014e00);
        assert_eq!(u64_at(0x873), 0x1588000);
        assert_eq!(u64_at(0xb84), 0xfffffc2000014ee0);
        assert_eq!(&bytes[0xb8c..0xb90], &[0, 1, 0, 0]);
        assert_eq!(u64_at(0xbb4), 0xfffffc2000014a60);
        assert_eq!(&bytes[0xbbc..0xbc0], &(-0x258i32).to_le_bytes());
        assert_eq!(u64_at(0xbc1), 0xfffffc20000148ac);
        assert_eq!(u64_at(0xbc9), 0xfffffc200001488c);
        assert_eq!(&bytes[0xbd4..0xbd8], &[2, 0, 0, 0x40]);
        assert!(bytes[0x140..0x720].iter().all(|byte| *byte == 0));
    }
    #[test]
    fn invalid_address_is_transactional() {
        let mut bytes = [0x55; SIZE];
        let mut a = addresses(); a.command_gpu |= 1;
        assert!(encode(&mut bytes, a, 16384, 512).is_err());
        assert!(bytes.iter().all(|byte| *byte == 0x55));
        assert!(encode(&mut bytes[..SIZE - 1], addresses(), 16384, 512).is_err());
    }
}
