// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J713 package-temperature cooling. PMP labels are not qualified GPU sensors.
//! The thermal governor requests cooling; an independent sensor check limits
//! rapid excursions and sensor failure before publishing firmware OPP caps.

use core::{ffi::{c_int, c_void}, ptr::NonNull,
    sync::atomic::{AtomicBool, AtomicU32, Ordering}};
use kernel::ffi::{c_long, c_ulong};
use kernel::{bindings, c_str, error::from_err_ptr, prelude::*};

static REGISTERED: AtomicBool = AtomicBool::new(false);
static COOLING_STATE: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn get_max(_: *mut bindings::thermal_cooling_device,
    state: *mut c_ulong) -> c_int {
    // SAFETY: The thermal core supplies a valid output pointer.
    unsafe { *state = 8 };
    0
}

unsafe extern "C" fn get_cur(_: *mut bindings::thermal_cooling_device,
    state: *mut c_ulong) -> c_int {
    // SAFETY: The thermal core supplies a valid output pointer.
    unsafe { *state = COOLING_STATE.load(Ordering::Relaxed) as c_ulong };
    0
}

unsafe extern "C" fn set_cur(_: *mut bindings::thermal_cooling_device,
    state: c_ulong) -> c_int {
    if state > 8 { return EINVAL.to_errno(); }
    COOLING_STATE.store(state as u32, Ordering::Release);
    0
}

static OPS: bindings::thermal_cooling_device_ops = bindings::thermal_cooling_device_ops {
    get_max_state: Some(get_max), get_cur_state: Some(get_cur), set_cur_state: Some(set_cur),
    get_requested_power: None, state2power: None, power2state: None,
};

pub(crate) struct Cooling {
    cdev: NonNull<bindings::thermal_cooling_device>,
    sram: NonNull<c_void>,
    policy: crate::g16_thermal_policy::Policy,
}

impl Cooling {
    pub(crate) fn new() -> Result<Self> {
        if REGISTERED.swap(true, Ordering::AcqRel) { return Err(EBUSY); }
        // SAFETY: Read-only mapping of the admitted J713 PMP SRAM. The C
        // decoder checks identifiers, validity, range and stable snapshots.
        let sram = unsafe { bindings::ioremap(0x380500000, 0xc0000) };
        let Some(sram) = NonNull::new(sram) else {
            REGISTERED.store(false, Ordering::Release);
            return Err(ENOMEM);
        };
        COOLING_STATE.store(8, Ordering::Release);
        // SAFETY: Static callbacks only access atomic state; unregister waits
        // for the thermal core before the module or mapping can disappear.
        let cdev = unsafe { bindings::thermal_cooling_device_register(
            c_str!("asahi-g16g").as_char_ptr(), core::ptr::null_mut(), &OPS) };
        let cdev = match from_err_ptr(cdev) {
            Ok(ptr) if !ptr.is_null() => NonNull::new(ptr).unwrap(),
            Ok(_) => {
                // SAFETY: No device was registered; release our mapping.
                unsafe { bindings::iounmap(sram.as_ptr()) };
                REGISTERED.store(false, Ordering::Release);
                return Err(ENOMEM);
            }
            Err(error) => {
                // SAFETY: This function still owns this unused mapping.
                unsafe { bindings::iounmap(sram.as_ptr()) };
                REGISTERED.store(false, Ordering::Release);
                return Err(error);
            }
        };
        Ok(Self { cdev, sram, policy: crate::g16_thermal_policy::Policy::new() })
    }

    /// Called under the runtime mutex before submission and while jobs run.
    /// Idle hardware needs no polling; the next submission samples afresh.
    pub(crate) fn limit(&mut self) -> u32 {
        let now = <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get();
        if self.policy.read_due(now) {
            let mut temp: c_long = 0;
            // SAFETY: The mapping and output remain valid throughout this read.
            let ret = unsafe { bindings::apple_pmp_temp_hotspot(self.sram.as_ptr(), &mut temp) };
            self.policy.sample(now, if ret == 0 { Some(temp as i32) } else { None });
        }
        self.policy.limit(COOLING_STATE.load(Ordering::Acquire))
    }
}

impl Drop for Cooling {
    fn drop(&mut self) {
        // SAFETY: Sole owner. Callbacks hold no references to the mapping.
        unsafe {
            bindings::thermal_cooling_device_unregister(self.cdev.as_ptr());
            bindings::iounmap(self.sram.as_ptr());
        }
        REGISTERED.store(false, Ordering::Release);
    }
}
