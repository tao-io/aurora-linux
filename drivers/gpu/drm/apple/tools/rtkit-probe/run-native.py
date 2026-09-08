#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Guard native DCP startup and retain its DMA allocations until reboot."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--guards", type=Path, required=True)
p.add_argument("--module", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
p.add_argument("--start-link", action="store_true")
a = p.parse_args()
sys.path.insert(0, str(a.guards))
from experiment_watchdog import ExperimentWatchdog
from experiment_thermals import ExperimentThermals

a.output.mkdir(parents=True, exist_ok=False)
w = ExperimentWatchdog()
r = {"watchdog": w.arm(), "start_link": a.start_link,
     "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}
t = ExperimentThermals(a.output)
try:
    t.start()
except BaseException:
    w.disarm()  # No hardware operation has started.
    raise
since = str(int(time.time()))
try:
    t.check()
    proc = subprocess.run(["insmod", str(a.module), f"start_link={int(a.start_link)}"],
                          capture_output=True, text=True, timeout=30)
    r.update(returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr)
    time.sleep(1)
    log = subprocess.check_output(["journalctl", "-k", "-b", "--since", "@" + since,
                                   "--no-pager", "-o", "cat"], text=True, timeout=5)
    (a.output / "kernel.log").write_text(log)
    if proc.returncode or "M4 native RTKit boot complete; running=1" not in log:
        raise RuntimeError("native RTKit did not reach running state")
    if "M4 native DCP diagnostic finished ret=0;" not in log:
        raise RuntimeError("native DCP startup failed")
    if a.start_link and "M4 DCPLink READY version=" not in log:
        raise RuntimeError("DCPLink did not reach READY")
    if "DCP firmware crashed" in log:
        raise RuntimeError("DCP firmware crash")
    t.check()
    r.update(passed=True, resources_retained_until_reboot=True)
except BaseException as error:
    r.update(passed=False, failure=repr(error))
    raise
finally:
    t.close()
    r["thermal"] = t.summary()
    (a.output / "result.json").write_text(json.dumps(r, indent=2) + "\n")
    os.sync()
w.disarm()
print(json.dumps(r, indent=2))
