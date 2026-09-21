// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Sensor-failure and rapid-rise backstop, independent of the thermal governor.
//! Standalone tests: rustc --test g16_thermal_policy.rs -o /tmp/g16-thermal-test.

pub(crate) struct Policy {
    last_ns: i64,
    previous_mc: Option<i32>,
    limited: bool,
    good_reads: u32,
    good_ns: i64,
    pub(crate) trace: Trace,
}

#[derive(Clone, Copy)]
pub(crate) struct Trace {
    pub(crate) now: i64,
    pub(crate) previous_mc: Option<i32>,
    pub(crate) elapsed: i64,
    pub(crate) projected: i64,
    pub(crate) reason: &'static str,
}

impl Policy {
    pub(crate) fn new() -> Self {
        Self { last_ns: 0, previous_mc: None, limited: true, good_reads: 0, good_ns: 0,
            trace: Trace { now: 0, previous_mc: None, elapsed: 0,
                projected: 0, reason: "startup" } }
    }

    pub(crate) fn read_due(&self, now: i64) -> bool {
        self.last_ns == 0 || now - self.last_ns >= 5_000_000
    }

    pub(crate) fn sample(&mut self, now: i64, temp: Option<i32>) {
        let elapsed = now - self.last_ns;
        let projected = match (temp, self.previous_mc) {
            (Some(t), Some(previous)) if elapsed > 0 && elapsed < 250_000_000 =>
                t as i64 + (t as i64 - previous as i64).max(0) * 100_000_000 / elapsed,
            (Some(t), _) => t as i64,
            _ => i64::MAX,
        };
        self.trace.now = now;
        self.trace.previous_mc = self.previous_mc;
        self.trace.elapsed = elapsed;
        self.trace.projected = projected;
        self.last_ns = now;
        self.previous_mc = temp;
        // Cold load steps are not a sustained heating slope. Predict only
        // near the passive trip, otherwise ordinary bursts lose performance.
        if temp.is_none_or(|t| t >= 92_000) ||
            (temp.is_some_and(|t| t >= 85_000) && projected >= 95_000) {
            self.limited = true;
            self.trace.reason = match temp {
                None => "sensor",
                Some(t) if t >= 92_000 => "hard-temperature",
                _ => "prediction",
            };
            self.good_reads = 0;
            self.good_ns = now;
        } else if temp.is_some_and(|t| t < 88_000) {
            // Faster hot sampling must not shorten the ~200ms cool recovery.
            if now - self.good_ns >= 20_000_000 {
                self.good_ns = now;
                self.good_reads = (self.good_reads + 1).min(10);
            }
            if self.good_reads == 10 {
                self.limited = false;
                self.trace.reason = "governor";
            }
        } else {
            self.good_reads = 0;
            self.good_ns = now;
        }
    }

    pub(crate) fn limit(&self, cooling_state: u32) -> u32 {
        if self.limited { 1 } else { 9 - cooling_state.min(8) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn cool(p: &mut Policy, start: i64) {
        for i in 1..=10 { p.sample(start + i * 20_000_000, Some(70_000)); }
    }
    #[test]
    fn five_ms_samples_do_not_accelerate_cool_recovery() {
        let mut p = Policy::new();
        for i in 1..40 {
            p.sample(i * 5_000_000, Some(70_000));
            assert_eq!(p.limit(0), 1);
        }
        p.sample(200_000_000, Some(70_000));
        assert_eq!(p.limit(0), 9);
        p.sample(205_000_000, Some(100_000));
        assert_eq!(p.limit(0), 1);
        for i in 1..40 {
            p.sample(205_000_000 + i * 5_000_000, Some(70_000));
            assert_eq!(p.limit(0), 1);
        }
        p.sample(405_000_000, Some(70_000));
        assert_eq!(p.limit(0), 9);
    }
    #[test]
    fn startup_and_sensor_failure_require_recovery() {
        let mut p = Policy::new();
        assert_eq!(p.limit(0), 1);
        cool(&mut p, 0);
        assert_eq!(p.limit(0), 9);
        p.sample(220_000_000, None);
        assert_eq!(p.limit(0), 1);
        cool(&mut p, 220_000_000);
        assert_eq!(p.limit(0), 9);
    }
    #[test]
    fn forecast_clamps_before_target() {
        let mut p = Policy::new();
        cool(&mut p, 0);
        p.sample(500_000_000, Some(86_000));
        assert_eq!(p.limit(0), 9); // Do not project across idle gaps.
        p.sample(520_000_000, Some(88_000));
        assert_eq!(p.limit(0), 1); // 98 C projected, 88 C measured.
    }
    #[test]
    fn cold_load_step_does_not_force_minimum_clocks() {
        let mut p = Policy::new();
        cool(&mut p, 0);
        p.sample(220_000_000, Some(80_000)); // Forecast 130 C is spurious.
        assert_eq!(p.limit(0), 9);
        p.sample(240_000_000, Some(85_000)); // Now close enough to intervene.
        assert_eq!(p.limit(0), 1);
    }
    #[test]
    fn hard_temperature_and_hysteresis() {
        let mut p = Policy::new();
        cool(&mut p, 0);
        p.sample(500_000_000, Some(92_000));
        assert_eq!(p.limit(0), 1);
        for i in 1..=9 { p.sample(500_000_000+i*20_000_000, Some(87_000)); }
        assert_eq!(p.limit(0), 1);
        p.sample(700_000_000, Some(89_000)); // Interrupt recovery.
        p.sample(720_000_000, Some(87_000));
        assert_eq!(p.limit(0), 1);
        cool(&mut p, 720_000_000);
        assert_eq!(p.limit(0), 9);
    }
    #[test]
    fn governor_always_caps_and_sampling_is_bounded() {
        let mut p = Policy::new();
        cool(&mut p, 0);
        assert_eq!(p.limit(3), 6);
        assert_eq!(p.limit(8), 1);
        assert!(!p.read_due(204_999_999));
        assert!(p.read_due(205_000_000));
    }
}
