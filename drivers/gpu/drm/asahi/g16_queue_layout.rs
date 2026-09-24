// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! J713 / 25G83 queue and client-notification storage. Values preserve the
//! qualified g16_queue producer (e2ff38a41660). This is not the G15 queue ABI.
//! Writes remain checked Buffer operations with explicit little-endian widths.
//! Names for unresolved fields identify their location, not guessed semantics.
pub(crate) mod context {
    pub const SIZE:usize=0x38;
    pub const REGISTRATION:usize=0; // Two slot bytes, then observed ready state 4.
    pub const ENABLE:usize=5;
    pub const SCHEDULING_WEIGHT:usize=0x26;
    pub const SENTINEL_33:usize=0x33;
}
pub(crate) mod info {
    pub const SIZE:usize=0x24c0; // Includes owned firmware scheduler storage.
    pub const RING_STATE:usize=0;
    pub const RING:usize=8;
    pub const NOTIFIER_LIST:usize=0x10;
    pub const SCHEDULER_POINTER:usize=0x18;
    pub const SCHEDULER_STORAGE:usize=0xb0;
    pub const CONSUMERS:[usize;3]=[0x20,0x24,0x28];
    pub const EVENT_ID:usize=0x2c;
    pub const PRIORITY_0:usize=0x30;
    pub const PRIORITY_1:usize=0x34;
    pub const PRIORITY_MASK:usize=0x38;
    pub const PRIORITY_5:usize=0x48;
    pub const UNKNOWN_4C:usize=0x4c;
    pub const STATE_80:usize=0x80; // Qualified idle predicate requires zero.
    pub const CONTEXT:usize=0xa4; // Packed u64 firmware VA.
}
pub(crate) mod ring {
    pub const SIZE:usize=0x60;
    pub const GPU_READ:usize=0x30;
    pub const CPU_WRITE:usize=0x40;
    pub const CAPACITY:usize=0x50;
    pub const ENTRY_SIZE:usize=8; // Firmware command VA, not a GPU VA.
}
pub(crate) mod notifier_list {
    pub const SIZE:usize=0x18;
    pub const SELF:usize=8; // Existing empty-list self link.
}
pub(crate) mod barrier {
    pub const SIZE:usize=0x40;
    pub const OPCODE:usize=0;
    pub const STAMP_0:usize=4;
    pub const STAMP_1:usize=0xc;
    pub const CONTROL_20:usize=0x20;
    // Preserve both references to the owned zero stamp and opaque control.
    pub const QUALIFIED_OPCODE:u32=4;
    pub const QUALIFIED_CONTROL:u32=0x80;
}
pub(crate) mod client {
    pub const SIZE:usize=0x1000;
    // Engine order: tiler, fragment, compute. Notification records precede
    // these u64 cells; firmware/user completion writes use their low u32.
    pub const THRESHOLDS:[usize;3]=[0x600,0x608,0x610];
    pub const FW_STAMPS:[usize;3]=[0x620,0x630,0x640];
    pub const USER_STAMPS:[usize;3]=[0x628,0x638,0x648];
}
