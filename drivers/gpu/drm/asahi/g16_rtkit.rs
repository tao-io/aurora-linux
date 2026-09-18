// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! RTKit callbacks for the identified J713 firmware. The firmware advertises
//! its crash storage by physical address inside the reserved data segment.

use crate::g16_resources::Region;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use kernel::{
    c_str,
    device::Core,
    sync::Completion,
    io::mem::{Mem, MemFlag},
    iosys_map::IoSysMapRef,
    platform,
    prelude::*,
    soc::apple::rtkit,
    sync::{aref::ARef, Arc, ArcBorrow},
};

/// IRQ-safe notification tokens for the single GPU completion worker.
#[pin_data]
pub(crate) struct EventWait {
    count: AtomicU64,
    #[pin]
    arrived: Completion,
}

impl EventWait {
    fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(EventWait {
                count: AtomicU64::new(0),
                arrived <- Completion::new(),
            }),
            GFP_KERNEL,
        )
    }

    fn record(&self) {
        self.count.fetch_add(1, Ordering::Release);
        self.arrived.complete();
    }

    /// Sleep until a notification later than `seen` has been recorded or
    /// `jiffies` elapse. Returns whether a notification or pending wakeup exists.
    pub(crate) fn wait_past(&self, seen: u64, jiffies: kernel::time::Jiffies) -> bool {
        // Events can arrive while the worker is polling. Discard stale wakeup
        // tokens before checking the sequence. A concurrent event either moves
        // the sequence or leaves a token, including between the check and wait.
        // Bound draining so a busy interrupt source cannot hold this worker.
        for _ in 0..64 {
            if !self.arrived.try_wait_for_completion() {
                if self.count.load(Ordering::Acquire) != seen { return true; }
                return self.arrived.wait_for_completion_timeout(jiffies);
            }
        }
        // Resnapshot GPU progress before draining another batch of tokens.
        true
    }
}

pub(crate) struct State {
    dev: ARef<platform::Device>,
    drm: crate::driver::AsahiDevRef,
    data: Region,
    pub(crate) health: Arc<Health>,
    claimed: AtomicBool,
    /// Firmware event notifications received on the application endpoint.
    /// The firmware sends one after a command's completion processing.
    pub(crate) event_messages: AtomicU64,
    pub(crate) events: Arc<EventWait>,
}

/// Shared status contains no device references, so DRM files may retain it
/// after runtime teardown without retaining MMIO or creating a DRM cycle.
pub(crate) struct Health {
    crashed: AtomicBool,
    mapped: AtomicBool,
    failed: AtomicBool,
}

impl Health {
    pub(crate) fn healthy(&self) -> bool {
        self.mapped.load(Ordering::Acquire)
            && !self.crashed.load(Ordering::Acquire)
            && !self.failed()
    }

    pub(crate) fn failed(&self) -> bool { self.failed.load(Ordering::Acquire) }

    pub(crate) fn mark_failed(&self) { self.failed.store(true, Ordering::Release); }
}

impl State {
    pub(crate) fn new(dev: &platform::Device<Core>, drm: crate::driver::AsahiDevRef, data: Region) -> Result<Arc<Self>> {
        Ok(Arc::new(
            Self {
                dev: dev.into(),
                drm,
                data,
                health: Arc::new(Health {
                    crashed: AtomicBool::new(false),
                    mapped: AtomicBool::new(false),
                    failed: AtomicBool::new(false),
                }, GFP_KERNEL)?,
                claimed: AtomicBool::new(false),
                event_messages: AtomicU64::new(0),
                events: EventWait::new()?,
            },
            GFP_KERNEL,
        )?)
    }

    pub(crate) fn healthy(&self) -> bool {
        self.health.healthy()
    }
}

pub(crate) struct FirmwareBuffer {
    state: Arc<State>,
    mapping: Mem,
    physical: usize,
    offset: usize,
    size: usize,
}

impl Drop for FirmwareBuffer {
    fn drop(&mut self) {
        // Includes allocation failure in RTKit's wrapper after shmem_map
        // returned: readiness must not survive loss of the actual mapping.
        self.state.health.mapped.store(false, Ordering::Release);
    }
}

impl rtkit::Buffer for FirmwareBuffer {
    fn iova(&self) -> Result<usize> {
        Ok(self.physical)
    }
    fn buf(&mut self) -> Result<IoSysMapRef<'_, u8>> {
        self.mapping.iosys_map(self.offset, self.size)
    }
}

pub(crate) struct Operations;

#[kernel::macros::vtable]
impl rtkit::Operations for Operations {
    type Data = Arc<State>;
    type Buffer = FirmwareBuffer;

    fn crashed(state: ArcBorrow<'_, State>, crashlog: Option<&[u8]>) {
        state.health.crashed.store(true, Ordering::Release);
        dev_err!(
            state.dev.as_ref(),
            "G16G: firmware crashed, retained crashlog bytes={}\n",
            crashlog.map_or(0, |b| b.len())
        );
    }

    fn shmem_map(
        state: ArcBorrow<'_, State>,
        physical: usize,
        size: usize,
    ) -> Result<FirmwareBuffer> {
        // The only system buffer advertised by this admitted firmware is its
        // preallocated crash log. Never reinterpret an arbitrary DVA as a PA.
        let offset = physical
            .checked_sub(state.data.base as usize)
            .ok_or(EINVAL)?;
        if size == 0
            || (physical | size) & 0xfff != 0
            || offset.checked_add(size).ok_or(EOVERFLOW)? > state.data.size as usize
        {
            return Err(EINVAL);
        }
        if state.claimed.swap(true, Ordering::AcqRel) {
            return Err(EBUSY);
        }
        let node = state.dev.as_ref().of_node().ok_or(ENODEV)?;
        let resource = node.reserved_mem_region_to_resource_byname(c_str!("fw-data"))?;
        if resource.start() != state.data.base || resource.size() != state.data.size {
            return Err(EINVAL);
        }
        // SAFETY: The validated no-map data reservation belongs to this
        // firmware session. This CPU mapping is retained by RTKit until ASC
        // is stopped and callbacks drained; physical pages are never freed.
        // An uncached CPU alias lets RTKit snapshot newly written crash data.
        let mapping = unsafe { Mem::try_new(resource, MemFlag::WC.into()) }?;
        dev_info!(
            state.dev.as_ref(),
            "G16G: mapped firmware crash storage PA={:#x} size={:#x}\n",
            physical,
            size
        );
        state.health.mapped.store(true, Ordering::Release);
        Ok(FirmwareBuffer {
            state: state.into(),
            mapping,
            physical,
            offset,
            size,
        })
    }

    fn recv_message_early(state: ArcBorrow<'_, State>, endpoint: u8, message: u64) -> bool {
        // Firmware retries its event notification until the shared rings are
        // drained, including after the final userspace job. Never take the
        // runtime mutex in the RTKit callback: enqueue the serialized worker.
        if endpoint == 0x20 && message == 0x0042_0000_0000_0000 {
            // Complete and queue_work are IRQ-safe. Avoid allocating an RTKit
            // receive work item and waiting for its worker before waking ours.
            state.events.record();
            state.event_messages.fetch_add(1, Ordering::Release);
            if crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
                dev_info!(state.dev.as_ref(), "G16G_TIMING event monotonic_ns={} messages={}\n",
                    <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get(),
                    state.event_messages.load(Ordering::Acquire));
            }
            crate::driver::queue_g16_completion_worker(state.drm.clone());
            return true;
        }
        false
    }

    fn recv_message(state: ArcBorrow<'_, State>, endpoint: u8, message: u64) {
        dev_warn!(
            state.dev.as_ref(),
            "G16G: RTKit application ep={:#x} message={:#018x}\n",
            endpoint,
            message
        );
    }
}
