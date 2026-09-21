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
    startup_limit_mc = 95000
    # Provisional development policy authorized 2026-09-21: regulate toward
    # 110 C, emergency stop at 120 C. Not an Apple hardware specification.
    stop_limit_mc = 120000
    interval_seconds = 0.5
    sync_interval_seconds = 5
    stale_seconds = 2

    def __init__(self, result, sysfs=Path('/sys'), on_trip=stop_workload):
        self.result = Path(result)
        self.sysfs = Path(sysfs)
        self.on_trip = on_trip
        self.sensors = []
        self.cpu_frequency_caps = []
        self.peaks = {}
        self.samples = 0
        self.last_sample = None
        self.last_sync = None
        self.failure = None
        self.stop_event = threading.Event()
        self.thread = None
        self.all_sensor_paths = {}
        self.all_thermals = {}
        self.all_peaks = {}

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

    def collect_all_thermals(self, readings):
        """Record all exposed hwmon/thermal inputs, without qualifying new gates.

        Stable path keys preserve duplicate labels. Rediscover hotplugged sensors
        and retain missing/error entries rather than silently dropping them.
        Only the established PMP channels drive the emergency stop policy.
        """
        for device in sorted((self.sysfs / 'class/hwmon').glob('hwmon*')):
            for path in sorted(device.glob('temp*_input')):
                self.all_sensor_paths[path] = (device / 'name',
                    path.with_name(path.name.replace('_input', '_label')))
        for device in sorted((self.sysfs / 'class/thermal').glob('thermal_zone*')):
            self.all_sensor_paths[device / 'temp'] = (device / 'type', device / 'type')
        pmp_readings = {path: readings[label] for path, label in zip(self.sensors, self.labels)}
        records = {}
        for path, (provider, label) in sorted(self.all_sensor_paths.items()):
            key = str(path.relative_to(self.sysfs))
            record = dict(self.all_thermals.get(key, {}))
            record.pop('temperature_mc', None)
            record.pop('error', None)
            record['sample_start_monotonic_seconds'] = time.monotonic()
            try:
                record['provider'] = provider.read_text().strip()
                record['label'] = label.read_text().strip() if label.exists() else path.stem
                value = pmp_readings[path] if path in pmp_readings else int(path.read_text())
                record['temperature_mc'] = value
                self.all_peaks[key] = max(value, self.all_peaks.get(key, value))
            except (OSError, ValueError) as error:
                record['error'] = repr(error)
            record['sample_end_monotonic_seconds'] = time.monotonic()
            records[key] = record
        self.all_thermals = records
        return records

    def sample(self):
        if (self.zone / 'mode').read_text().strip() != 'enabled':
            raise RuntimeError('CPU thermal policy is disabled')
        readings = {label: int(path.read_text()) for label, path in zip(self.labels, self.sensors)}
        if any(value < 0 or value > 125000 for value in readings.values()):
            raise RuntimeError(f'invalid PMP temperature readings: {readings}')
        record = {'monotonic_seconds': time.monotonic(), 'unix_seconds': time.time(),
                  'temperatures_mc': readings, 'cpu_policies': {}}
        critical = max(readings.values()) >= self.stop_limit_mc
        # Never delay a known emergency with optional SMC bus transactions.
        record['all_thermals'] = self.all_thermals if critical else self.collect_all_thermals(readings)
        record['all_thermals_cached'] = critical
        for p in (self.sysfs / 'devices/system/cpu/cpufreq').glob('policy*'):
            record['cpu_policies'][p.name] = {
                name: int((p / name).read_text()) for name in ('scaling_cur_freq', 'scaling_max_freq')}
        for label, value in readings.items():
            self.peaks[label] = max(value, self.peaks.get(label, value))
        with (self.result / 'temperatures.jsonl').open('a') as log:
            log.write(json.dumps(record) + '\n')
            log.flush()
            if critical or self.last_sync is None or time.monotonic() - self.last_sync >= self.sync_interval_seconds:
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
                'all_peak_temperatures_mc': dict(self.all_peaks),
                'dedicated_gpu_sensor_verified': False, 'log': 'temperatures.jsonl',
                'cpu_frequency_caps': self.cpu_frequency_caps}

    def cap_cpu_frequency(self, requested_khz):
        """Lower test clocks after the caller arms its watchdog; never raise them.

        Keep the cap until reboot, including after a failed experiment. Restoring
        high clocks in error cleanup could increase heat with a stuck workload.
        """
        if requested_khz <= 0:
            raise ValueError('CPU test frequency cap must be positive')
        policies = sorted((self.sysfs / 'devices/system/cpu/cpufreq').glob('policy*'))
        if not policies:
            raise RuntimeError('CPU frequency policies unavailable')
        for policy in policies:
            maximum = policy / 'scaling_max_freq'
            minimum = policy / 'scaling_min_freq'
            old_max, old_min = int(maximum.read_text()), int(minimum.read_text())
            available = [int(f) for f in (policy / 'scaling_available_frequencies').read_text().split()]
            if available and requested_khz >= max(available):
                # scaling_max_freq includes the temporary thermal QoS ceiling.
                # Writing it back would turn throttling into a persistent user
                # cap. A full-frequency run needs no additional cap: hardware
                # already bounds the clock, and lower user limits stay intact.
                self.cpu_frequency_caps.append({
                    'policy': policy.name, 'old_min_khz': old_min,
                    'old_max_khz': old_max, 'max_khz': max(available),
                    'effective_max_khz': old_max, 'cap_applied': False})
                (self.result / 'cpu-frequency-caps.json').write_text(
                    json.dumps(self.cpu_frequency_caps, indent=2) + '\n')
                continue
            candidates = [f for f in available if f <= min(requested_khz, old_max)]
            if not candidates:
                raise RuntimeError(f'no supported CPU frequency below requested cap: {policy}')
            target = max(candidates)
            if old_min > target:
                minimum.write_text(str(target))
            maximum.write_text(str(target))
            actual = int(maximum.read_text())
            # cpufreq applies its QoS update on a worker, after sysfs write returns.
            deadline = time.monotonic() + 1
            while actual > target and time.monotonic() < deadline:
                time.sleep(0.01)
                actual = int(maximum.read_text())
            self.cpu_frequency_caps.append({'policy': policy.name, 'old_min_khz': old_min,
                                            'old_max_khz': old_max, 'max_khz': actual})
            (self.result / 'cpu-frequency-caps.json').write_text(
                json.dumps(self.cpu_frequency_caps, indent=2) + '\n')
            if actual > target:
                raise RuntimeError(f'CPU frequency cap not applied: {policy}')

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
        if os.environ.get('M4_TEST_CPU_MAX_KHZ'):
            self.cap_cpu_frequency(int(os.environ['M4_TEST_CPU_MAX_KHZ']))
        record = self.sample()
        if max(record['temperatures_mc'].values()) >= self.startup_limit_mc:
            raise RuntimeError('let the die cool below 95 C before starting a GPU experiment')
        self.thread = threading.Thread(target=self.monitor, name='experiment-thermals', daemon=True)
        self.thread.start()
        return self.summary()

    def check(self):
        if self.failure:
            raise RuntimeError('thermal guard stopped this experiment: ' + self.failure)
        if not self.thread or not self.thread.is_alive() or self.last_sample is None or \
                time.monotonic() - self.last_sample > self.stale_seconds:
            raise RuntimeError('thermal monitor stopped or its readings are stale')
        for cap in self.cpu_frequency_caps:
            maximum = self.sysfs / 'devices/system/cpu/cpufreq' / cap['policy'] / 'scaling_max_freq'
            if int(maximum.read_text()) > cap['max_khz']:
                self.trip(RuntimeError('CPU experiment frequency cap was removed'))
                raise RuntimeError('CPU experiment frequency cap was removed')

    def close(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        path = self.result / 'temperatures.jsonl'
        if path.exists():
            with path.open('rb') as log:
                os.fdatasync(log.fileno())
