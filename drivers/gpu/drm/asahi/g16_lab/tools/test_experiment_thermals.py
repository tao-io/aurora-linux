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
        (self.pmp / 'temp1_input').write_text('65000')
        with self.assertRaisesRegex(RuntimeError, 'cool'):
            self.guard.start()
        self.assertIsNone(self.guard.thread)
        (self.pmp / 'temp1_input').unlink()
        with self.assertRaises(OSError):
            self.guard.start()

    def test_every_sensor_trips_at_exact_cutoff(self):
        self.guard.discover()
        for i in range(1, 7):
            path = self.pmp / f'temp{i}_input'
            path.write_text('75000')
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
        (self.pmp / 'temp5_input').write_text('76000')
        self.guard.thread.join(timeout=1)
        self.assertEqual(self.stops, [True])
        self.assertIsNotNone(self.guard.failure)

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


if __name__ == '__main__':
    unittest.main()
