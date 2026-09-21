// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Byte-bounded storage for fully retired, displaced command owners.
//! Firmware's last owner and in-flight commands must never enter this cache.
use kernel::prelude::*;

pub(crate) const CACHE_BYTES: usize = 128 * 1024 * 1024;

pub(crate) struct OwnerCache<T> {
    entries: KVec<(usize, T)>,
    bytes: usize,
}

impl<T> OwnerCache<T> {
    pub(crate) fn new() -> Self {
        Self { entries: KVec::new(), bytes: 0 }
    }

    pub(crate) fn take(&mut self, matches: impl Fn(&T) -> bool) -> Option<T> {
        let index = self.entries.iter().position(|(_, owner)| matches(owner))?;
        let (bytes, owner) = self.entries.remove(index).ok()?;
        self.bytes -= bytes;
        Some(owner)
    }

    pub(crate) fn put(&mut self, owner: T, bytes: usize) {
        // Large scenes remain legal; they simply bypass the idle cache.
        // Subtraction also avoids overflowing when given a very large owner.
        if bytes > CACHE_BYTES { return; }
        while self.bytes > CACHE_BYTES - bytes {
            let Ok((old_bytes, old)) = self.entries.remove(0) else { return; };
            self.bytes -= old_bytes;
            drop(old);
        }
        if self.entries.push((bytes, owner), GFP_KERNEL).is_ok() {
            self.bytes += bytes;
        }
    }
}
