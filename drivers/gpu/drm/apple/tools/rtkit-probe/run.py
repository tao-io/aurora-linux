#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run a bounded DCP diagnostic using the existing G16 watchdog/thermal guards."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--guards", required=True, type=Path)
parser.add_argument("--module", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--power-on", action="store_true")
args = parser.parse_args()
sys.path.insert(0, str(args.guards))
from experiment_watchdog import ExperimentWatchdog
from experiment_thermals import ExperimentThermals

args.output.mkdir(parents=True, exist_ok=False)
watchdog = ExperimentWatchdog()
record = {"watchdog": watchdog.arm(), "power_on": args.power_on,
          "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}
thermal = ExperimentThermals(args.output)
try:
    thermal.start()
except BaseException:
    watchdog.disarm()  # No hardware operation has started.
    raise
started = str(int(time.time()))
try:
    thermal.check()
    proc = subprocess.run(["insmod", str(args.module),
                           f"power_on={int(args.power_on)}"],
                          capture_output=True, text=True, timeout=10)
    record.update(returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr)
    log = subprocess.check_output(["journalctl", "-k", "-b", "--since", "@" + started,
                                   "--no-pager", "-o", "cat"], text=True, timeout=5)
    (args.output / "kernel.log").write_text(log)
    if proc.returncode:
        raise RuntimeError(f"probe failed: {proc.stderr.strip()}; watchdog remains armed")
    if args.power_on and "inherited RTKit acknowledged IOP power ON" not in log:
        raise RuntimeError("probe did not record power acknowledgement")
    subprocess.run(["rmmod", "m4_dcp_probe"], check=True, timeout=5)
    thermal.check()
    record["passed"] = True
except BaseException as error:
    record.update(passed=False, failure=repr(error))
    raise
finally:
    thermal.close()
    record["thermal"] = thermal.summary()
    (args.output / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    os.sync()
watchdog.disarm()
print(json.dumps(record, indent=2))
