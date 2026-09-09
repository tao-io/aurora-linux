// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! RTKit callbacks for the identified J713 firmware. The firmware advertises
//! its crash storage by physical address inside the reserved data segment.

use crate::g16_resources::Region;
use core::sync::atomic::{AtomicBool, Ordering};
use kernel::{
    c_str,
    device::Core,
    io::mem::{Mem, MemFlag},
    iosys_map::IoSysMapRef,
    platform,
    prelude::*,
    soc::apple::rtkit,
    sync::{aref::ARef, Arc, ArcBorrow},
};

pub(crate) struct State {
    dev: ARef<platform::Device>,
    data: Region,
    crashed: AtomicBool,
    mapped: AtomicBool,
    claimed: AtomicBool,
}

impl State {
    pub(crate) fn new(dev: &platform::Device<Core>, data: Region) -> Result<Arc<Self>> {
        Ok(Arc::new(
            Self {
                dev: dev.into(),
                data,
                crashed: AtomicBool::new(false),
                mapped: AtomicBool::new(false),
                claimed: AtomicBool::new(false),
            },
            GFP_KERNEL,
        )?)
    }

    pub(crate) fn healthy(&self) -> bool {
        self.mapped.load(Ordering::Acquire) && !self.crashed.load(Ordering::Acquire)
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
        self.state.mapped.store(false, Ordering::Release);
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
        state.crashed.store(true, Ordering::Release);
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
        state.mapped.store(true, Ordering::Release);
        Ok(FirmwareBuffer {
            state: state.into(),
            mapping,
            physical,
            offset,
            size,
        })
    }

    fn recv_message(state: ArcBorrow<'_, State>, endpoint: u8, message: u64) {
        dev_info!(
            state.dev.as_ref(),
            "G16G: RTKit application ep={:#x} message={:#018x}\n",
            endpoint,
            message
        );
    }
}
