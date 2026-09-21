// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! CPU and GPU share the tested package-temperature backstop in pmp-cooling.h.
//! The GPU combines its independent sample with the thermal governor ceiling.

use kernel::bindings;

pub(crate) struct Policy {
    state: bindings::apple_pmp_cooling,
}

pub(crate) struct Trace {
    pub(crate) now: i64,
    pub(crate) previous_mc: Option<i32>,
    pub(crate) elapsed: i64,
    pub(crate) projected: i64,
    pub(crate) reason: &'static str,
    pub(crate) hard: bool,
    pub(crate) prediction: u32,
}

impl Policy {
    pub(crate) fn new() -> Self {
        Self { state: bindings::apple_pmp_cooling {
            last_ns: 0, good_ns: 0, anchor_ns: 0, release_ns: 0, dt_ns: 0,
            anchor_mc: 0, previous_mc: 0, projected_mc: 0, good_reads: 0,
            prediction: 0, level: 8, hard: true, anchor_valid: false, reason: 0,
        } }
    }

    pub(crate) fn read_due(&self, now: i64) -> bool {
        self.state.last_ns == 0 || now as u64 - self.state.last_ns >= 5_000_000
    }

    pub(crate) fn sample(&mut self, now: i64, temp: Option<i32>) {
        // SAFETY: The shared policy contains only scalars, is owned by this
        // object, and is sampled under the runtime mutex. Temperature comes
        // from the validated PMP decoder; None must retain fault protection.
        unsafe { bindings::apple_pmp_cooling_sample(&mut self.state, now as u64,
            temp.unwrap_or(0) as _, i32::from(temp.is_none())) };
    }

    pub(crate) fn trace(&self) -> Trace {
        let s = &self.state;
        Trace { now: s.last_ns as i64, previous_mc: Some(s.previous_mc as i32),
            elapsed: s.dt_ns as i64, projected: s.projected_mc as i64,
            hard: s.hard, prediction: s.prediction,
            reason: match s.reason {
                0 => "startup", 1 => "sensor", 2 => "measured-heat",
                3 => "prediction", 5 => "recovery", _ => "governor",
            } }
    }

    pub(crate) fn limit(&mut self, cooling_state: u32) -> u32 {
        // SAFETY: Owned scalar policy, serialized by the GPU runtime mutex.
        let now = self.state.last_ns;
        unsafe { bindings::apple_pmp_cooling_observe(&mut self.state, now, cooling_state) };
        9 - cooling_state.min(8).max(self.state.level.min(8))
    }
}
