// SPDX-License-Identifier: GPL-2.0-only
//! Typed M4/25G83 render envelopes and firmware sequences.
//! Wire evidence: g16_lab/render/g16_render_pack.h and its producer references.
//! All addresses are supplied by GEM owners. No captured graph is instantiated.

use crate::g16_render::{BuildError, Stage};

#[derive(Clone, Copy)]
pub(crate) struct Args {
    pub command: u64, pub sku: u64, pub queue: u64, pub stats: u64,
    pub pb_slot: u64, pub manager: u64, pub pb_aux: u64,
    pub notifier: u64, pub shared_stamp: u64, pub stamp_address: u64,
    pub uma: u64, pub uma_aux: u64, pub timestamp_storage: u64,
    pub context: u32, pub generation: u8, pub counter: u64,
    pub uuid: u32, pub stamp: u32, pub event_slot: u32,
    pub fragment_slot: u32, pub fragment_stamp: u32,
    pub pb_id: u32, pub pass_id: u32,
}
// Normal single-pass mode in matched 25G83 J713 captures. The native producer
// selects 1/2/3; zero is not its normal render mode. This is separate from
// the ISP raster mode register. Keep command, start and finalize in agreement.
const RENDER_MODE: u8 = 1;

// Compute and render use the same qualified 25G83 timestamp operation.
pub(crate) use crate::g16_compute::timestamp;

/// Per-render-owner firmware metadata allocation. These are subrecords of
/// one retained Buffer; aliases are supplied by that owner. Both timestamp
/// pairs are distinct from optional user destination pairs and fanout storage.
/// Legacy stamp cells remain diagnostic storage; live completion uses the
/// queue-owned notification stamps. No allocation or scheduling policy changes.
pub(crate) mod metadata {
    pub const SIZE: usize = 0x4000;
    pub const SCRATCH_GPU: usize = 0;
    pub const SCRATCH_FW: usize = 8;
    pub const COMPACT_SCENE_LIST: usize = 0x28;
    pub const STATISTICS_POINTER: usize = 0x40;
    pub const PB_AUX: usize = 0x100;
    pub const STATISTICS: usize = 0x200;
    pub const INIT_BUFFER: usize = 0x300;
    pub const TILER_DEPENDENCY: usize = 0x340;
    pub const COMPUTE_DEPENDENCY: usize = 0x380;
    pub const LEGACY_FW_STAMPS: [usize; 2] = [0x400, 0x410];
    pub const LEGACY_USER_STAMPS: [usize; 2] = [0x408, 0x418];
    pub const PRIOR_FRAGMENT: usize = 0x440;
    pub const UMA_SUBMITTED: usize = 0x500;
    pub const TIME_PAIRS: [usize; 2] = [0x600, 0x620];
    pub const USER_TIME_PAIRS: [usize; 2] = [0x610, 0x630];
    pub const FANOUT_PAIRS: usize = 0x700;
    pub const UMA_AUX: [usize; 2] = [0x100, 0x200];
}
/// Parameter manager byte layout, J713/25G83. Widths are explicit in the
/// encoder; firmware/GPU aliases and physical slot identity are distinct inputs.
pub(crate) mod parameter {
    pub const SIZE: usize = 0xc0;
    pub const HARDWARE_ID: usize = 0xc;
    pub const ID: usize = 0x10;
    pub const PAGES_FW: usize = 0x20;
    pub const PAGES_GPU: usize = 0x28;
    pub const LIST_BYTES: usize = 0x30;
    pub const PAGE_COUNT: usize = 0x34;
    pub const BLOCK_CAPACITY: usize = 0x38;
    pub const WRITE: usize = 0x3c;
    pub const READ: usize = 0x40;
    pub const BLOCKS: usize = 0x44;
    pub const RING: usize = 0x4c;
    pub const LAST_PAGE: usize = 0x54;
    pub const BLOCK_BYTES: usize = 0x58;
    pub const COUNTER: usize = 0x64;
    pub const MAX_PAGES: usize = 0x7c;
    pub const MIN_PAGES: usize = 0x80;
    pub const DISCARD: usize = 0x84;
}
pub(crate) mod dependency {
    pub const SIZE: usize = 0x40;
    pub const KIND: usize = 0;
    pub const STAMP: usize = 4;
    pub const STAMP_COPY: usize = 0xc;
    pub const WAIT_VALUE: usize = 0x14;
    pub const WAIT_SLOT: usize = 0x20;
    pub const SELF_VALUE: usize = 0x24;
    pub const INTERNAL: usize = 0x3c;
}
pub(crate) mod init_buffer {
    pub const SIZE: usize = 0x20;
    pub const KIND: usize = 0;
    pub const CONTEXT: usize = 4;
    pub const ID: usize = 8;
    pub const CURSOR: usize = 0xc;
    pub const STAGED: usize = 0x10;
    pub const MANAGER: usize = 0x14;
    pub const STAMP: usize = 0x1c;
}

/// Engine-specific offsets for the qualified J713/25G83 render envelope.
/// Command offsets and microsequence offsets are deliberately separate names.
/// Encoding, timestamp patching and relocation all consume this definition.
pub(crate) struct Layout {
    pub command_sequence: usize,
    pub command_sequence_size: usize,
    pub timestamp_start: usize,
    pub finalize: usize,
    pub finalize_restart: usize,
    pub finalize_uma_pointer: usize,
    pub finalize_opcode: u32,
    pub uma_header: usize,
    pub sequence_size: usize,
    pub command_flush_stamps: usize,
    pub command_status: usize,
    pub attachments: usize,
    pub finalize_has_attachments: usize,
    finish: usize,
    job_meta: usize,
    command_time_control: usize,
    command_time_pair: usize,
    command_time_auxiliary: usize,
    command_context_time: usize,
}

pub(crate) const fn layout(stage: Stage) -> Layout {
    match stage {
        Stage::Tiling => Layout {
            command_sequence: 0x770, command_sequence_size: 0x778,
            timestamp_start: 0x1cc, finalize: 0x268, finalize_restart: 0x80,
            finalize_uma_pointer: 0x78, finalize_opcode: 6, uma_header: 0x190,
            command_flush_stamps: 0x878 + 0x1c, command_status: 0x918,
            attachments: 0x88, finalize_has_attachments: 0x2ec,
            sequence_size: 0x300, finish: 0x94, job_meta: 0x878,
            command_time_control: 0x8c0, command_time_pair: 0x8c8,
            command_time_auxiliary: 0x928, command_context_time: 0x83c,
        },
        Stage::Fragment => Layout {
            command_sequence: 0x14, command_sequence_size: 0x1c,
            timestamp_start: 0x1ec, finalize: 0x288, finalize_restart: 0xa0,
            finalize_uma_pointer: 0x98, finalize_opcode: 8, uma_header: 0x1b0,
            command_flush_stamps: 0xbc0 + 0x1c, command_status: 0xc60,
            attachments: 0xa8, finalize_has_attachments: 0x32c,
            sequence_size: 0x380, finish: 0xbc, job_meta: 0xbc0,
            command_time_control: 0xc08, command_time_pair: 0xc10,
            command_time_auxiliary: 0xc70, command_context_time: 0xb84,
        },
    }
}

fn put(out: &mut [u8], offset: usize, value: u64, size: usize) {
    out[offset..offset + size].copy_from_slice(&value.to_le_bytes()[..size]);
}
fn valid_fw(address: u64) -> bool {
    address >= 0xffff_fc00_0000_0000 && address <= u64::MAX - 0x4000 && address & 3 == 0
}

pub(crate) fn sequence_size(stage: Stage) -> usize {
    layout(stage).sequence_size
}

pub(crate) fn encode(command: &mut [u8], sku: &mut [u8], stage: Stage, a: Args) -> Result<(), BuildError> {
    let ta = stage == Stage::Tiling;
    let wire = layout(stage);
    let (start, finish, uma, meta) = (wire.timestamp_start, wire.finish, wire.uma_header, wire.job_meta);
    let total = sequence_size(stage);
    if command.len() < stage.command_size() || sku.len() < total { return Err(BuildError::BufferTooSmall); }
    if a.context >= 64 || a.generation == 0 || a.event_slot >= 128 || a.stamp == 0
        || a.fragment_slot >= 128 || a.fragment_stamp == 0
        || ![a.command, a.sku, a.queue, a.stats, a.pb_slot, a.manager, a.pb_aux,
              a.notifier, a.shared_stamp, a.stamp_address, a.uma,
              a.timestamp_storage].iter().all(|p| valid_fw(*p))
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
            (wire.command_sequence, a.sku, 8), (wire.command_sequence_size, total as u64, 4), (0x8a0, a.pass_id as u64, 4)] {
            put(command, off, value, size);
        }
        command[0x917] = a.generation;
        // Native submitBuffer 8a0306c copies the peer fragment event here.
        // Firmware 588c/58cc uses its slot and stamp generation to schedule
        // a partial fragment when TA exhausts parameter pages. Zero names
        // an unrelated event, leaving TA waiting for a fragment it cannot run.
        put(command, 0x77c, a.fragment_slot as u64, 4);
        put(command, 0x780, a.fragment_stamp as u64, 4);
    } else {
        for (off, value, size) in [
            (wire.command_sequence, a.sku, 8), (wire.command_sequence_size, total as u64, 4), (0x20, a.notifier, 8),
            (0x28, a.manager, 8), (0x30, a.pb_slot, 8), (0x38, a.pb_aux, 8),
            (0xbe8, a.pass_id as u64, 4)] { put(command, off, value, size); }
        command[0xc5f] = a.generation;
        command[0xc84] = RENDER_MODE;
    }
    put(sku, 0, if ta { 5 } else { 7 }, 4);
    // Completion must flush the same cache generation that starts this work.
    put(sku, if ta { 0x4c } else { 0x64 }, crate::g16_compute::EVENT_GENERATION as u64, 4);
    put(sku, 0x14, a.command + if ta { 0x40 } else { 0x80 }, 8);
    put(sku, uma, a.uma, 8);
    put(sku, uma + 0x18, a.uma_aux, 8);
    // The native producer mirrors RunCommand+4 into the start record.
    // TA+1b8 / Fragment+1d8 are not padding in the UMA-adjacent tail.
    put(sku, uma + 0x28, a.counter, 8);
    // J713/25G83 encodeTimeStamp emits these 0x4c-byte records around WFI.
    // Pointer fields name cells in RunCommand, not their eventual destinations.
    // Keep both output cells in the pass owner's retained metadata allocation.
    let (control, pointers, auxiliary, context_time) = (wire.command_time_control,
        wire.command_time_pair, wire.command_time_auxiliary, wire.command_context_time);
    put(command, pointers, a.timestamp_storage, 8);
    put(command, pointers + 8, a.timestamp_storage + 8, 8);
    for (offset, opcode, selected) in [(start, 0x80000003, pointers),
                                      (start + timestamp::END_DELTA, 3, pointers + 8)] {
        let timestamp = &mut sku[offset..offset + timestamp::SIZE];
        put(timestamp, 0, opcode, 4);
        for (off, value) in [(timestamp::CONTROL, a.command + control as u64),
            (timestamp::INTERNAL_PAIR, a.command + pointers as u64),
            (timestamp::SELECTED_CELL, a.command + selected as u64), (timestamp::QUEUE, a.queue),
            (timestamp::AUXILIARY, a.command + auxiliary as u64),
            (timestamp::CONTEXT_TIME, a.command + context_time as u64), (timestamp::UUID, a.uuid as u64)] {
            put(timestamp, off, value, 8);
        }
    }
    put(sku, start + timestamp::SIZE, 1, 4);
    if ta {
        for (off, value, size) in [
            (0x1c,a.manager,8), (0x24,a.pb_slot,8), (0x2c,a.stats,8),
            (0x34,a.queue,8), (0x3c,a.command+0x85c,8), (0x44,a.context as u64,4),
            (0x48,1,4), (0x50,a.pb_id as u64,4), (0x58,a.pass_id as u64,4),
            (0x64,a.command+0x784,8), (0x6c,a.command+0x8a8,8),
            (0x7c,a.uuid as u64,4), (0x1c0,a.event_slot as u64,4)] { put(sku,off,value,size); }
    } else {
        for (off, value, size) in [
            (0x1c,a.pb_slot,8), (0x24,a.stats,8), (0x2c,a.command+0xb70,8),
            (0x34,a.pb_slot+0x54,8), (0x3c,a.command+0xbb0,8), (0x44,a.command+0xbb4,8),
            (0x4c,a.queue,8), (0x54,a.command,8), (0x5c,a.context as u64,4),
            (0x60,1,4), (0x68,a.pb_id as u64,4), (0x70,a.pass_id as u64,4),
            (0x7c,a.command+0xa58,8), (0x84,a.command+0xbf0,8),
            (0xa0,a.uuid as u64,4), (0x1e0,a.event_slot as u64,4)] { put(sku,off,value,size); }
    }
    if !ta { sku[0x1e4] = RENDER_MODE; }
    let finalize = wire.finalize;
    let f = &mut sku[finalize..];
    put(f, 0, wire.finalize_opcode as u64, 4);
    if ta {
        for (off,value,size) in [
            (4,a.pb_slot,8), (0xc,a.manager,8), (0x14,a.stats,8), (0x1c,a.queue,8),
            (0x24,a.command+0x85c,8), (0x2c,a.context as u64,4),
            (0x34,a.command+0x784,8), (0x40,a.uuid as u64,4), (0x48,a.stamp_address,8),
            (0x50,a.stamp as u64,4), (wire.finalize_uma_pointer,a.sku+uma as u64,8),
            (wire.finalize_restart,(-(finalize as i32)) as u32 as u64,4), (0x85,a.command+wire.command_status as u64,8)] { put(f,off,value,size); }
    } else {
        for (off,value,size) in [
            (4,a.uuid as u64,4), (0xc,a.stamp_address,8), (0x14,a.stamp as u64,4),
            (0x1c,a.pb_slot,8), (0x24,a.manager,8), (0x2c,1,4), (0x30,a.stats,8),
            (0x38,a.command+0xbb0,8), (0x40,a.command+0xbb4,8), (0x48,a.command+0xb70,8),
            (0x50,a.queue,8), (0x58,a.command,8), (0x60,a.context as u64,4),
            (0x64,a.command+0xa58,8), (wire.finalize_uma_pointer,a.sku+uma as u64,8),
            (wire.finalize_restart,(-(finalize as i32)) as u32 as u64,4), (0xa5,a.command+wire.command_status as u64,8),
            (0xad,1,4), (0xb2,a.event_slot as u64,4)] { put(f,off,value,size); }
    }
    f[if ta { 0x8d } else { 0xb1 }] = RENDER_MODE;
    put(f,finish,0x40000002,4);
    Ok(())
}

/// Relocate a complete native program while preserving all external pointers.
/// Finalize's UMA header pointer is the only absolute self-reference; its
/// relative restart and repeated timestamp records remain byte-identical.
pub(crate) fn relocate_program(sequence: &mut [u8], stage: Stage, address: u64) -> Result<(), BuildError> {
    let base = sequence_size(stage);
    if !valid_fw(address) || sequence.len() < base || sequence.len() > 0x800 ||
        (sequence.len() - base) % timestamp::SIZE != 0 ||
        (stage == Stage::Tiling && sequence.len() != base) { return Err(BuildError::Address); }
    let extra = sequence.len() - base;
    let wire = layout(stage);
    let (finalize, restart, pointer, uma, opcode) = (wire.finalize + extra,
        wire.finalize_restart, wire.finalize_uma_pointer, wire.uma_header as u64, wire.finalize_opcode);
    if sequence[finalize..finalize+4] != opcode.to_le_bytes() ||
        sequence[finalize+restart..finalize+restart+4] != (-(finalize as i32)).to_le_bytes() {
        return Err(BuildError::Address);
    }
    put(sequence, finalize + pointer, address + uma, 8);
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
    if out.len() < parameter::SIZE { return Err(BuildError::BufferTooSmall); }
    if ![a.pages_fw,a.blocks,a.ring,a.counter].iter().all(|p| valid_fw(*p))
        || a.pages_gpu == 0 || a.pages_gpu >= 1 << 43 || a.pages == 0 || a.pages & 3 != 0
        || a.pages > a.list_bytes / 4 || a.pages / 4 >= a.block_capacity
        || a.write >= a.block_capacity || a.read >= a.block_capacity
        || a.min_pages > a.max_pages || a.max_pages > a.list_bytes / 4 { return Err(BuildError::Address); }
    out[..parameter::SIZE].fill(0);
    // Full InitBuffer rebuild indexes hardware descriptors from manager+0xc
    // (25G83 TEXT 2eb70/2ec90), not InitBuffer+8 or RunTA+18. The runtime
    // reserves one physical slot per in-flight owner; bind that slot here.
    // Leaving this zero aliases every owner's page list in descriptor zero,
    // which serial rendering hides but overlapping TA/FS can corrupt.
    for (off,value,size) in [
        (parameter::HARDWARE_ID,a.id as u64,4), (parameter::ID,a.id as u64,4),
        (parameter::PAGES_FW,a.pages_fw,8), (parameter::PAGES_GPU,a.pages_gpu,8),
        (parameter::LIST_BYTES,a.list_bytes as u64,4), (parameter::PAGE_COUNT,a.pages as u64,4), (parameter::BLOCK_CAPACITY,a.block_capacity as u64,4),
        (parameter::WRITE,a.write as u64,4), (parameter::READ,a.read as u64,4), (parameter::BLOCKS,a.blocks,8), (parameter::RING,a.ring,8),
        (parameter::LAST_PAGE,(a.pages-1) as u64,4), (parameter::BLOCK_BYTES,0x20000,4), (parameter::COUNTER,a.counter,8),
        (parameter::MAX_PAGES,a.max_pages as u64,4), (parameter::MIN_PAGES,a.min_pages as u64,4), (parameter::DISCARD,a.discard,8)] { put(out,off,value,size); }
    Ok(())
}
pub(crate) fn init_parameter_buffer(out: &mut [u8], context: u32, id: u32,
    cursor: u32, staged: u32, manager: u64, stamp: u32) -> Result<(), BuildError> {
    if out.len() < init_buffer::SIZE { return Err(BuildError::BufferTooSmall); }
    if context >= 64 || !valid_fw(manager) { return Err(BuildError::Address); }
    for (off,value,size) in [(init_buffer::KIND,6,4), (init_buffer::CONTEXT,context as u64,4), (init_buffer::ID,id as u64,4),
        (init_buffer::CURSOR,cursor as u64,4), (init_buffer::STAGED,staged as u64,4), (init_buffer::MANAGER,manager,8), (init_buffer::STAMP,stamp as u64,4)] {
        put(out,off,value,size);
    }
    Ok(())
}

/// M4/25G83 AGXChannel::setupBarrierCommand (+0x896b734), consumed by
/// agx_drain_work_queue_commands (+0x1cd20). A slot dependency lets firmware
/// schedule partial fragments before the final tiling stamp is written.
pub(crate) fn render_dependency(out: &mut [u8], stamp_address: u64,
    wait_slot: u8, wait_value: u32, stamp_self: u32) -> Result<(), BuildError> {
    if out.len() < dependency::SIZE { return Err(BuildError::BufferTooSmall); }
    if !valid_fw(stamp_address) || wait_slot >= 128 || wait_value == 0 || stamp_self == 0 {
        return Err(BuildError::Address);
    }
    out[..dependency::SIZE].fill(0);
    put(out, dependency::KIND, 4, 4);
    put(out, dependency::STAMP, stamp_address, 8);
    put(out, dependency::STAMP_COPY, stamp_address, 8);
    put(out, dependency::WAIT_VALUE, wait_value as u64, 4);
    put(out, dependency::WAIT_SLOT, wait_slot as u64, 4);
    put(out, dependency::SELF_VALUE, stamp_self as u64, 4);
    Ok(())
}

/// Repeat the native end-timestamp operation before fragment finalization.
/// 25G83 TEXT 19364..19524 reads the completed engine timestamps (c028/c038)
/// and writes the same nanosecond conversion to each user pointer pair. No
/// new engine command may start between these records. Keep the finalize
/// restart branch relative to the moved record for partial-render retries.
pub(crate) fn fragment_timestamp_copies(sequence: &mut [u8], pointers: &[u64])
    -> Result<usize, BuildError> {
    const END: usize = layout(Stage::Fragment).timestamp_start + timestamp::END_DELTA;
    const FINALIZE: usize = layout(Stage::Fragment).finalize;
    const RECORD: usize = timestamp::SIZE;
    let base = sequence_size(Stage::Fragment);
    let extra = pointers.len() * RECORD;
    if pointers.len() > 8 || !pointers.iter().all(|p| valid_fw(*p)) {
        return Err(BuildError::Address);
    }
    if sequence.len() < base + extra { return Err(BuildError::BufferTooSmall); }
    if pointers.is_empty() { return Ok(base); }
    let mut timestamp = [0; RECORD];
    timestamp.copy_from_slice(&sequence[END..FINALIZE]);
    sequence.copy_within(FINALIZE..base, FINALIZE + extra);
    for (i, pointer) in pointers.iter().enumerate() {
        let record = &mut sequence[FINALIZE + i * RECORD..FINALIZE + (i + 1) * RECORD];
        record.copy_from_slice(&timestamp);
        put(record, timestamp::USER_PAIR, *pointer, 8);
    }
    put(sequence, FINALIZE + extra + layout(Stage::Fragment).finalize_restart,
        (-(FINALIZE as i32 + extra as i32)) as u32 as u64, 4);
    Ok(base + extra)
}
