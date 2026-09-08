#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only PMP monitoring with a latched stop for bounded J713 experiments.

These conservative experiment limits are not Apple hardware safety limits.
PMP labels do not establish a dedicated GPU temperature sensor. This monitor
does not control clocks, replace CPU thermal policy, or feed the watchdog.
"""
import json
import os
from pathlib import Path
import subprocess
import threading
import time


def stop_workload():
    Path('/run/m4-hyprland-stop').touch()
    subprocess.run(['systemctl', '--no-block', 'stop', 'm4-supertux.service',
                    'm4-supertuxkart.service'],
                   timeout=2, check=False)
    # The service accepts this only for the DRM frontend. Bound the write:
    # a wedged job must still be recovered by the independently armed watchdog.
    if Path('/sys/kernel/debug/g16_lab_render_submit').exists():
        subprocess.run(['python3', '-c',
                        'from pathlib import Path; '
                        'Path("/sys/kernel/debug/g16_lab_render_submit").write_text("stop\\n")'],
                       timeout=2, check=False)


class ExperimentThermals:
    labels = ('PMP aggregate die hotspot', 'Ta000m', 'Te000m', 'Te001m', 'Tp000m', 'Tp001m')
    startup_limit_mc = 65000
    stop_limit_mc = 75000
    interval_seconds = 0.5
    sync_interval_seconds = 5
    stale_seconds = 2

    def __init__(self, result, sysfs=Path('/sys'), on_trip=stop_workload):
        self.result = Path(result)
        self.sysfs = Path(sysfs)
        self.on_trip = on_trip
        self.sensors = []
        self.peaks = {}
        self.samples = 0
        self.last_sample = None
        self.last_sync = None
        self.failure = None
        self.stop_event = threading.Event()
        self.thread = None

    def discover(self):
        devices = [p for p in (self.sysfs / 'class/hwmon').glob('hwmon*')
                   if (p / 'name').read_text().strip() == 'apple_pmp']
        if len(devices) != 1:
            raise RuntimeError('expected exactly one apple_pmp temperature provider')
        self.sensors = [devices[0] / f'temp{i}_input' for i in range(1, 7)]
        labels = tuple((devices[0] / f'temp{i}_label').read_text().strip() for i in range(1, 7))
        if labels != self.labels:
            raise RuntimeError(f'unqualified PMP temperature labels: {labels}')
        zones = [p for p in (self.sysfs / 'class/thermal').glob('thermal_zone*')
                 if (p / 'type').read_text().strip() == 'apple-pmp-die']
        if len(zones) != 1:
            raise RuntimeError('CPU apple-pmp-die thermal policy is missing')
        self.zone = zones[0]

    def sample(self):
        if (self.zone / 'mode').read_text().strip() != 'enabled':
            raise RuntimeError('CPU thermal policy is disabled')
        readings = {label: int(path.read_text()) for label, path in zip(self.labels, self.sensors)}
        if any(value < 0 or value > 125000 for value in readings.values()):
            raise RuntimeError(f'invalid PMP temperature readings: {readings}')
        record = {'monotonic_seconds': time.monotonic(), 'unix_seconds': time.time(),
                  'temperatures_mc': readings, 'cpu_policies': {}}
        for p in (self.sysfs / 'devices/system/cpu/cpufreq').glob('policy*'):
            record['cpu_policies'][p.name] = {
                name: int((p / name).read_text()) for name in ('scaling_cur_freq', 'scaling_max_freq')}
        for label, value in readings.items():
            self.peaks[label] = max(value, self.peaks.get(label, value))
        with (self.result / 'temperatures.jsonl').open('a') as log:
            log.write(json.dumps(record) + '\n')
            log.flush()
            if self.last_sync is None or time.monotonic() - self.last_sync >= self.sync_interval_seconds:
                os.fdatasync(log.fileno())
                self.last_sync = time.monotonic()
        self.samples += 1
        self.last_sample = record['monotonic_seconds']
        if max(readings.values()) >= self.stop_limit_mc:
            raise RuntimeError(f'temperature reached experiment cutoff {self.stop_limit_mc} mC: {readings}')
        return record

    def summary(self):
        return {'startup_limit_mc': self.startup_limit_mc, 'stop_limit_mc': self.stop_limit_mc,
                'interval_seconds': self.interval_seconds, 'samples': self.samples,
                'sync_interval_seconds': self.sync_interval_seconds,
                'peak_temperatures_mc': dict(self.peaks), 'failure': self.failure,
                'dedicated_gpu_sensor_verified': False, 'log': 'temperatures.jsonl'}

    def trip(self, error):
        # Latch before attempting any teardown so no further keepalive is allowed.
        self.failure = repr(error)
        print('Thermal experiment stop: ' + self.failure, flush=True)
        try:
            # Stop first: slow or failed storage must not delay this request.
            self.on_trip()
        finally:
            with (self.result / 'thermal-stop.json').open('w') as log:
                json.dump(self.summary(), log, indent=2)
                log.flush()
                os.fsync(log.fileno())

    def monitor(self):
        while not self.stop_event.wait(self.interval_seconds):
            try:
                self.sample()
            except Exception as error:
                self.trip(error)
                return

    def start(self):
        self.discover()
        record = self.sample()
        if max(record['temperatures_mc'].values()) >= self.startup_limit_mc:
            raise RuntimeError('let the die cool below 65 C before starting a GPU experiment')
        self.thread = threading.Thread(target=self.monitor, name='experiment-thermals', daemon=True)
        self.thread.start()
        return self.summary()

    def check(self):
        if self.failure:
            raise RuntimeError('thermal guard stopped this experiment: ' + self.failure)
        if not self.thread or not self.thread.is_alive() or self.last_sample is None or \
                time.monotonic() - self.last_sample > self.stale_seconds:
            raise RuntimeError('thermal monitor stopped or its readings are stale')

    def close(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        path = self.result / 'temperatures.jsonl'
        if path.exists():
            with path.open('rb') as log:
                os.fdatasync(log.fileno())
