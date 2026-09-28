#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise safety decisions against temporary sysfs files, never live hardware."""
import json
from pathlib import Path
import tempfile
import time
import unittest
from experiment_thermals import ExperimentThermals


class ThermalsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.pmp = self.root / 'class/hwmon/hwmon7'
        self.pmp.mkdir(parents=True)
        (self.pmp / 'name').write_text('apple_pmp\n')
        for i, label in enumerate(ExperimentThermals.labels, 1):
            (self.pmp / f'temp{i}_label').write_text(label)
            (self.pmp / f'temp{i}_input').write_text('57000')
        self.zone = self.root / 'class/thermal/thermal_zone3'
        self.zone.mkdir(parents=True)
        (self.zone / 'type').write_text('apple-pmp-die')
        (self.zone / 'mode').write_text('enabled')
        self.stops = []
        self.guard = ExperimentThermals(self.root, self.root, lambda: self.stops.append(True))

    def tearDown(self):
        self.guard.close()
        self.temp.cleanup()

    def test_healthy_readings_and_peaks(self):
        self.guard.start()
        self.guard.check()
        (self.pmp / 'temp6_input').write_text('62000')
        self.guard.sample()
        self.assertEqual(self.guard.summary()['peak_temperatures_mc']['Tp001m'], 62000)
        record = json.loads((self.root / 'temperatures.jsonl').read_text().splitlines()[-1])
        self.assertEqual(record['temperatures_mc']['Tp001m'], 62000)
        self.assertFalse(self.stops)

    def test_startup_hot_or_missing_sensor_refuses(self):
        (self.pmp / 'temp1_input').write_text('95000')
        with self.assertRaisesRegex(RuntimeError, 'cool below'):
            self.guard.start()
        self.assertIsNone(self.guard.thread)
        (self.pmp / 'temp1_input').unlink()
        with self.assertRaises(OSError):
            self.guard.start()

    def test_all_sensors_duplicate_labels_and_hotplug_are_recorded(self):
        self.guard.discover()
        for n, value in [(8, '33200'), (9, '44000')]:
            device = self.root / f'class/hwmon/hwmon{n}'
            device.mkdir()
            (device / 'name').write_text('macsmc_hwmon')
            (device / 'temp1_label').write_text('Same label')
            (device / 'temp1_input').write_text(value)
        (self.zone / 'temp').write_text('58000')
        record = self.guard.sample()
        sensors = record['all_thermals']
        self.assertEqual(sensors['class/hwmon/hwmon8/temp1_input']['temperature_mc'], 33200)
        self.assertEqual(sensors['class/hwmon/hwmon9/temp1_input']['temperature_mc'], 44000)
        self.assertEqual(sensors['class/thermal/thermal_zone3/temp']['temperature_mc'], 58000)
        (self.root / 'class/hwmon/hwmon8/temp1_input').unlink()
        (self.root / 'class/hwmon/hwmon9/temp2_input').write_text('-1000')
        sensors = self.guard.sample()['all_thermals']
        self.assertIn('error', sensors['class/hwmon/hwmon8/temp1_input'])
        self.assertNotIn('temperature_mc', sensors['class/hwmon/hwmon8/temp1_input'])
        self.assertEqual(sensors['class/hwmon/hwmon9/temp2_input']['temperature_mc'], -1000)

    def test_critical_sample_does_not_wait_for_optional_sensors(self):
        self.guard.discover()
        self.guard.sample()
        previous = self.guard.all_thermals
        def fail_if_called(readings):
            self.fail('known emergency must not wait for optional sensors')
        self.guard.collect_all_thermals = fail_if_called
        (self.pmp / 'temp1_input').write_text('120000')
        with self.assertRaisesRegex(RuntimeError, 'cutoff'):
            self.guard.sample()
        record = json.loads((self.root / 'temperatures.jsonl').read_text().splitlines()[-1])
        self.assertTrue(record['all_thermals_cached'])
        self.assertEqual(record['all_thermals'], previous)

    def test_supplemental_read_error_is_evidence_not_a_new_trip_policy(self):
        self.guard.discover()
        device = self.root / 'class/hwmon/hwmon8'
        device.mkdir()
        (device / 'name').write_text('macsmc_hwmon')
        (device / 'temp1_input').write_text('bad')
        record = self.guard.sample()
        self.assertIn('error', record['all_thermals']['class/hwmon/hwmon8/temp1_input'])
        self.assertFalse(self.stops)

    def test_every_sensor_trips_at_exact_cutoff(self):
        self.guard.discover()
        for i in range(1, 7):
            path = self.pmp / f'temp{i}_input'
            path.write_text('120000')
            with self.assertRaisesRegex(RuntimeError, 'cutoff'):
                self.guard.sample()
            path.write_text('57000')

    def test_monitor_latches_sensor_loss_and_stops(self):
        self.guard.interval_seconds = 0.01
        self.guard.start()
        (self.pmp / 'temp3_input').unlink()
        self.guard.thread.join(timeout=1)
        self.assertEqual(self.stops, [True])
        with self.assertRaisesRegex(RuntimeError, 'stopped this experiment'):
            self.guard.check()
        self.assertIsNotNone(json.loads((self.root / 'thermal-stop.json').read_text())['failure'])

    def test_monitor_latches_hot_reading_and_stops(self):
        self.guard.interval_seconds = 0.01
        self.guard.start()
        (self.pmp / 'temp5_input').write_text('120000')
        self.guard.thread.join(timeout=1)
        self.assertEqual(self.stops, [True])
        self.assertIsNotNone(self.guard.failure)

    def test_kernel_control_headroom_is_not_an_emergency_stop(self):
        self.guard.start()
        (self.pmp / 'temp5_input').write_text('119999')
        self.guard.sample()
        self.guard.check()
        self.assertFalse(self.stops)

    def test_disabled_policy_invalid_and_stale_readings(self):
        self.guard.start()
        (self.zone / 'mode').write_text('disabled')
        with self.assertRaisesRegex(RuntimeError, 'disabled'):
            self.guard.sample()
        (self.zone / 'mode').write_text('enabled')
        (self.pmp / 'temp2_input').write_text('-1')
        with self.assertRaisesRegex(RuntimeError, 'invalid'):
            self.guard.sample()
        self.guard.last_sample = time.monotonic() - 3
        with self.assertRaisesRegex(RuntimeError, 'stale'):
            self.guard.check()

    def test_log_failure_still_stops_workload(self):
        (self.root / 'thermal-stop.json').mkdir()
        with self.assertRaises(OSError):
            self.guard.trip(RuntimeError('sensor failure'))
        self.assertEqual(self.stops, [True])
        self.assertIsNotNone(self.guard.failure)

    def test_full_clocks_do_not_latch_a_thermal_ceiling(self):
        policy = self.root / 'devices/system/cpu/cpufreq/policy0'
        policy.mkdir(parents=True)
        (policy / 'scaling_available_frequencies').write_text('900000 1800000 4000000')
        (policy / 'cpuinfo_max_freq').write_text('4000000')
        (policy / 'scaling_min_freq').write_text('900000')
        (policy / 'scaling_cur_freq').write_text('900000')
        maximum = policy / 'scaling_max_freq'
        maximum.write_text('900000')
        before = maximum.stat().st_mtime_ns
        self.guard.cap_cpu_frequency(4104000)
        self.assertEqual(maximum.stat().st_mtime_ns, before)
        self.assertEqual(maximum.read_text(), '900000')
        self.assertFalse(self.guard.cpu_frequency_caps[0]['cap_applied'])
        self.guard.start()
        # Thermal QoS can recover without this being an overridden test cap.
        maximum.write_text('4000000')
        self.guard.check()
        self.assertFalse(self.stops)

    def test_ordinary_maximum_still_caps_enabled_boost(self):
        policy = self.root / 'devices/system/cpu/cpufreq/policy0'
        policy.mkdir(parents=True)
        (policy / 'scaling_available_frequencies').write_text('912000 1800000 4104000')
        (policy / 'cpuinfo_max_freq').write_text('4464000')
        (policy / 'scaling_min_freq').write_text('912000')
        (policy / 'scaling_cur_freq').write_text('912000')
        maximum = policy / 'scaling_max_freq'
        maximum.write_text('4464000')
        self.guard.cap_cpu_frequency(4104000)
        self.assertEqual(maximum.read_text(), '4104000')
        self.guard.start()
        self.guard.check()
        self.assertFalse(self.stops)
        maximum.write_text('4464000')
        with self.assertRaisesRegex(RuntimeError, 'cap was removed'):
            self.guard.check()
        self.assertEqual(self.stops, [True])

    def test_full_boost_request_keeps_thermal_recovery_unpinned(self):
        policy = self.root / 'devices/system/cpu/cpufreq/policy0'
        policy.mkdir(parents=True)
        (policy / 'scaling_available_frequencies').write_text('912000 1800000 4104000')
        (policy / 'cpuinfo_max_freq').write_text('4464000')
        (policy / 'scaling_min_freq').write_text('912000')
        (policy / 'scaling_cur_freq').write_text('912000')
        maximum = policy / 'scaling_max_freq'
        maximum.write_text('1800000')
        before = maximum.stat().st_mtime_ns
        self.guard.cap_cpu_frequency(4464000)
        self.assertEqual(maximum.stat().st_mtime_ns, before)
        self.assertFalse(self.guard.cpu_frequency_caps[0]['cap_applied'])
        self.guard.start()
        maximum.write_text('4464000')
        self.guard.check()
        self.assertFalse(self.stops)

    def test_cpu_cap_never_raises_existing_policy_and_detects_override(self):
        policies = self.root / 'devices/system/cpu/cpufreq'
        for name, minimum, maximum in [('policy0', 900000, 4000000),
                                       ('policy1', 900000, 900000)]:
            policy = policies / name
            policy.mkdir(parents=True)
            (policy / 'scaling_available_frequencies').write_text('900000 1800000 4000000')
            (policy / 'cpuinfo_max_freq').write_text('4000000')
            (policy / 'scaling_min_freq').write_text(str(minimum))
            (policy / 'scaling_max_freq').write_text(str(maximum))
            (policy / 'scaling_cur_freq').write_text(str(minimum))
        self.guard.cap_cpu_frequency(2000000)
        self.assertEqual((policies / 'policy0/scaling_max_freq').read_text(), '1800000')
        self.assertEqual((policies / 'policy1/scaling_max_freq').read_text(), '900000')
        self.guard.start()
        self.guard.check()
        (policies / 'policy0/scaling_max_freq').write_text('4000000')
        with self.assertRaisesRegex(RuntimeError, 'cap was removed'):
            self.guard.check()
        self.assertEqual(self.stops, [True])


if __name__ == '__main__':
    unittest.main()
