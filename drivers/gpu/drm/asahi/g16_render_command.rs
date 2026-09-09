// SPDX-License-Identifier: GPL-2.0-only
//! Typed M4/25G83 render envelopes and firmware sequences.
//! Wire evidence: g16_lab/render/g16_render_pack.h and its producer references.
//! All addresses are supplied by GEM owners. No captured graph is instantiated.

use crate::g16_render::{BuildError, Stage};

#[derive(Clone, Copy)]
pub(crate) struct Args {
    pub command: u64, pub sku: u64, pub queue: u64, pub slots: u64,
    pub pb_slot: u64, pub manager: u64, pub pb_aux: u64,
    pub notifier: u64, pub shared_stamp: u64, pub stamp_address: u64,
    pub uma: u64, pub uma_aux: u64,
    pub context: u32, pub generation: u8, pub counter: u64,
    pub uuid: u32, pub stamp: u32, pub event_slot: u32,
    pub pb_id: u32, pub pass_id: u32,
}
fn put(out: &mut [u8], offset: usize, value: u64, size: usize) {
    out[offset..offset + size].copy_from_slice(&value.to_le_bytes()[..size]);
}
fn valid_fw(address: u64) -> bool {
    address >= 0xffff_fc00_0000_0000 && address <= u64::MAX - 0x4000 && address & 3 == 0
}

pub(crate) fn sequence_size(stage: Stage) -> usize {
    match stage { Stage::Tiling => 0x280, Stage::Fragment => 0x2c0 }
}

pub(crate) fn encode(command: &mut [u8], sku: &mut [u8], stage: Stage, a: Args) -> Result<(), BuildError> {
    let ta = stage == Stage::Tiling;
    let (start, finish, uma, meta) = if ta { (0x1cc, 0x94, 0x190, 0x878) }
                                   else { (0x1ec, 0xbc, 0x1b0, 0xbc0) };
    let total = sequence_size(stage);
    if command.len() < stage.command_size() || sku.len() < total { return Err(BuildError::BufferTooSmall); }
    if a.context >= 64 || a.generation == 0 || a.event_slot >= 128 || a.stamp == 0
        || ![a.command, a.sku, a.queue, a.slots, a.pb_slot, a.manager, a.pb_aux,
              a.notifier, a.shared_stamp, a.stamp_address, a.uma].iter().all(|p| valid_fw(*p))
        || a.uma_aux == 0 || a.uma_aux >= 1 << 42 {
        return Err(BuildError::Address);
    }
    command[..stage.command_size()].fill(0);
    sku[..total].fill(0);
    put(command, 0, u64::from(!ta), 4);
    put(command, 4, a.counter, 8);
    put(command, 0xc, a.context as u64, 4);
    for (off, value, size) in [
        (0, a.shared_stamp, 8), (8, a.stamp_address, 8), (0x10, a.stamp as u64, 4),
        (0x14, a.event_slot as u64, 4), (0x20, a.uuid as u64, 4)] {
        put(command, meta + off, value, size);
    }
    if ta {
        for (off, value, size) in [
            (0x10, a.notifier, 8), (0x18, a.pb_id as u64, 4),
            (0x20, a.manager, 8), (0x28, a.pb_slot, 8), (0x30, a.pb_aux, 8),
            (0x770, a.sku, 8), (0x778, total as u64, 4), (0x8a0, a.pass_id as u64, 4)] {
            put(command, off, value, size);
        }
        command[0x917] = a.generation;
    } else {
        for (off, value, size) in [
            (0x14, a.sku, 8), (0x1c, total as u64, 4), (0x20, a.notifier, 8),
            (0x28, a.manager, 8), (0x30, a.pb_slot, 8), (0x38, a.pb_aux, 8),
            (0xbe8, a.pass_id as u64, 4)] { put(command, off, value, size); }
        command[0xc5f] = a.generation;
    }
    put(sku, 0, if ta { 5 } else { 7 }, 4);
    put(sku, 0x14, a.command + if ta { 0x40 } else { 0x80 }, 8);
    put(sku, uma, a.uma, 8);
    put(sku, uma + 0x18, a.uma_aux, 8);
    put(sku, start, 1, 4);
    if ta {
        for (off, value, size) in [
            (0x1c,a.manager,8), (0x24,a.pb_slot,8), (0x2c,a.slots,8),
            (0x34,a.queue,8), (0x3c,a.command+0x85c,8), (0x44,a.context as u64,4),
            (0x50,a.pb_id as u64,4), (0x58,a.pass_id as u64,4),
            (0x64,a.command+0x784,8), (0x6c,a.command+0x8a8,8),
            (0x7c,a.uuid as u64,4), (0x1c0,a.event_slot as u64,4)] { put(sku,off,value,size); }
    } else {
        for (off, value, size) in [
            (0x1c,a.pb_slot,8), (0x24,a.slots,8), (0x2c,a.command+0xb70,8),
            (0x34,a.pb_slot+0x54,8), (0x3c,a.command+0xbb0,8), (0x44,a.command+0xbb4,8),
            (0x4c,a.queue,8), (0x54,a.command,8), (0x5c,a.context as u64,4),
            (0x68,a.pb_id as u64,4), (0x70,a.pass_id as u64,4),
            (0x7c,a.command+0xa58,8), (0x84,a.command+0xbf0,8),
            (0xa0,a.uuid as u64,4), (0x1e0,a.event_slot as u64,4)] { put(sku,off,value,size); }
    }
    let f = &mut sku[start + 4..];
    put(f, 0, if ta { 6 } else { 8 }, 4);
    if ta {
        for (off,value,size) in [
            (4,a.pb_slot,8), (0xc,a.manager,8), (0x14,a.slots,8), (0x1c,a.queue,8),
            (0x24,a.command+0x85c,8), (0x2c,a.context as u64,4),
            (0x34,a.command+0x784,8), (0x40,a.uuid as u64,4), (0x48,a.stamp_address,8),
            (0x50,a.stamp as u64,4), (0x78,a.sku+uma as u64,8),
            (0x80,(-(start as i32+4)) as u32 as u64,4), (0x85,a.command+0x918,8)] { put(f,off,value,size); }
    } else {
        for (off,value,size) in [
            (4,a.uuid as u64,4), (0xc,a.stamp_address,8), (0x14,a.stamp as u64,4),
            (0x1c,a.pb_slot,8), (0x24,a.manager,8), (0x2c,1,4), (0x30,a.slots,8),
            (0x38,a.command+0xbb0,8), (0x40,a.command+0xbb4,8), (0x48,a.command+0xb70,8),
            (0x50,a.queue,8), (0x58,a.command,8), (0x60,a.context as u64,4),
            (0x64,a.command+0xa58,8), (0x98,a.sku+uma as u64,8),
            (0xa0,(-(start as i32+4)) as u32 as u64,4), (0xa5,a.command+0xc60,8),
            (0xad,1,4), (0xb2,a.event_slot as u64,4)] { put(f,off,value,size); }
    }
    put(f,finish,0x40000002,4);
    Ok(())
}

#[derive(Clone, Copy)]
pub(crate) struct ParameterManager {
    pub pages_fw: u64, pub pages_gpu: u64, pub blocks: u64, pub ring: u64,
    pub counter: u64, pub discard: u64, pub list_bytes: u32, pub pages: u32,
    pub block_capacity: u32, pub write: u32, pub read: u32,
    pub max_pages: u32, pub min_pages: u32, pub id: u32,
}
pub(crate) fn parameter_manager(out: &mut [u8], a: ParameterManager) -> Result<(), BuildError> {
    if out.len() < 0xc0 { return Err(BuildError::BufferTooSmall); }
    if ![a.pages_fw,a.blocks,a.ring,a.counter].iter().all(|p| valid_fw(*p))
        || a.pages_gpu == 0 || a.pages_gpu >= 1 << 43 || a.pages == 0 || a.pages & 3 != 0
        || a.pages > a.list_bytes / 4 || a.pages / 4 >= a.block_capacity
        || a.write >= a.block_capacity || a.read >= a.block_capacity
        || a.min_pages > a.max_pages || a.max_pages > a.list_bytes / 4 { return Err(BuildError::Address); }
    out[..0xc0].fill(0);
    for (off,value,size) in [
        (0x10,a.id as u64,4), (0x20,a.pages_fw,8), (0x28,a.pages_gpu,8),
        (0x30,a.list_bytes as u64,4), (0x34,a.pages as u64,4), (0x38,a.block_capacity as u64,4),
        (0x3c,a.write as u64,4), (0x40,a.read as u64,4), (0x44,a.blocks,8), (0x4c,a.ring,8),
        (0x54,(a.pages-1) as u64,4), (0x58,0x20000,4), (0x64,a.counter,8),
        (0x7c,a.max_pages as u64,4), (0x80,a.min_pages as u64,4), (0x84,a.discard,8)] { put(out,off,value,size); }
    Ok(())
}
pub(crate) fn init_parameter_buffer(out: &mut [u8], context: u32, id: u32,
    cursor: u32, staged: u32, manager: u64, stamp: u32) -> Result<(), BuildError> {
    if out.len() < 0x20 { return Err(BuildError::BufferTooSmall); }
    if context >= 64 || !valid_fw(manager) { return Err(BuildError::Address); }
    for (off,value,size) in [(0,6,4), (4,context as u64,4), (8,id as u64,4),
        (0xc,cursor as u64,4), (0x10,staged as u64,4), (0x14,manager,8), (0x1c,stamp as u64,4)] {
        put(out,off,value,size);
    }
    Ok(())
}

/// M4/25G83 AGXChannel::setupBarrierCommand (+0x896b734), consumed by
/// agx_drain_work_queue_commands (+0x1cd20). A slot dependency lets firmware
/// schedule partial fragments before the final tiling stamp is written.
pub(crate) fn render_dependency(out: &mut [u8], stamp_address: u64,
    wait_slot: u8, wait_value: u32, stamp_self: u32) -> Result<(), BuildError> {
    if out.len() < 0x40 { return Err(BuildError::BufferTooSmall); }
    if !valid_fw(stamp_address) || wait_slot >= 128 || wait_value == 0 || stamp_self == 0 {
        return Err(BuildError::Address);
    }
    out[..0x40].fill(0);
    put(out, 0, 4, 4);
    put(out, 4, stamp_address, 8);
    put(out, 0xc, stamp_address, 8);
    put(out, 0x14, wait_value as u64, 4);
    put(out, 0x20, wait_slot as u64, 4);
    put(out, 0x24, stamp_self as u64, 4);
    Ok(())
}
