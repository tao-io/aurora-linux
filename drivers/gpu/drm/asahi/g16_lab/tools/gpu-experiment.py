#!/usr/bin/env python3
"""Run one bounded J713 experiment through the existing mandatory guard."""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
HOST = "root@omarchy-m4"
SSH = ["ssh", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=10",
       "-o", "ServerAliveCountMax=3", HOST]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def experiment_output(directory):
    """Only logs retained by this experiment, including app-observe's child."""
    output = "\n".join(p.read_text(errors="replace") for p in (
        directory / "remote" / "command.log",
        directory / "remote" / "application" / "application.log",
    ) if p.exists())
    # muvm's PTY may prefix a result with a shell-integration OSC marker.
    # Classify visible text while preserving the original files as evidence.
    return re.sub(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b\[[0-?]*[ -/]*[@-~]", "", output)


def summarize_evidence(directory):
    """Compact observations only; never substitute for guard validation."""
    remote = directory / "remote"
    output = experiment_output(directory)
    observations = {}
    if output:
        lines = output.splitlines()
        pixels = [line for line in lines if re.search(
            r"\bbad=\d+(?:/\d+)?\b|^(?:PASS|FAIL):|\bpixels(?: checked)?, \d+ failures$|^pixels=\d+ incorrect=\d+|\bpixel \d+ differs at byte \d+", line)]
        observations["pixel_results"] = pixels[-32:]
        observations["pixel_result_count"] = len(pixels)
    thermal = remote / "temperatures.jsonl"
    if thermal.exists():
        peaks = {}
        samples = malformed = 0
        for line in thermal.read_text(errors="replace").splitlines():
            try:
                values = json.loads(line)["temperatures_mc"]
                if not isinstance(values, dict) or not values or not all(
                        isinstance(v, (int, float)) and not isinstance(v, bool)
                        for v in values.values()):
                    raise ValueError("invalid temperatures")
                for name, value in values.items():
                    peaks[name] = max(peaks.get(name, value), value)
                samples += 1
            except (ValueError, KeyError, TypeError):
                malformed += 1
        observations["thermal_samples"] = samples
        observations["malformed_thermal_samples"] = malformed
        observations["peak_temperature_c"] = max(peaks.values()) / 1000 if peaks else None
        observations["channel_peaks_c"] = {k: v / 1000 for k, v in peaks.items()}
    return observations


def source_state(repo):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(repo), *args])
    patch = git("diff", "HEAD", "--binary")
    return {"head": git("rev-parse", "HEAD").decode().strip(),
            "status": git("status", "--short").decode(),
            "patch_sha256": hashlib.sha256(patch).hexdigest()}, patch


def classify(directory, facts, expected):
    """Never turn a clean process exit or an old boot's fault into a verdict."""
    stdout = experiment_output(directory)
    # A repaired driver may succeed at a call that failed in the retained
    # capture. Preserve that warning in evidence without calling it a new error.
    # GFXReconstruct versions differ on whether they print a function name;
    # match the explicit actual/captured result pair, not the log prefix.
    stdout = re.sub(r"returned error value VK_SUCCESS that does not match the result from the capture file: VK_ERROR_[A-Z_]+\.", "", stdout, flags=re.M)
    kernel = directory / "remote" / "kernel-journal.log"
    if not kernel.exists() or not kernel.stat().st_size:
        kernel = directory / "remote" / "dmesg.log"
    current = []
    if kernel.exists():
        lines = kernel.read_text(errors="replace").splitlines()
        # kmsg and CLOCK_MONOTONIC are different clock domains on this port.
        # Use the paired marker from this exact probe when available.
        marker = re.search(r"M4 probe start monotonic=([0-9.]+)", stdout)
        marker_line = next((i for i, line in enumerate(lines)
                            if marker and marker[0] in line), None)
        if marker_line is not None:
            lines = lines[marker_line:]
        for line in lines:
            stamp = re.match(r"\[\s*([0-9.]+)\]", line)
            if stamp and (marker_line is not None or
                          float(stamp[1]) >= facts["uptime_seconds"]):
                current.append(line)
    kernels = "\n".join(current)
    # The follow-new stream is scoped to this experiment and is authoritative
    # when the journal collection stalls during recovery. A partial journal
    # can contain only old boot messages and must not hide a captured fault.
    live = directory / "remote" / "kernel-live.log"
    if live.exists():
        kernels += "\n" + live.read_text(errors="replace")
    if "Out of memory: Killed process" in kernels:
        return "host_out_of_memory"
    if "userspace GPU timeout" in kernels:
        return "gpu_timeout"
    if re.search(r"GPU retirement unproven|first runtime error|firmware crashed|Internal error:|BUG:|Kernel panic", kernels):
        return "gpu_fault"
    if re.search(r"M4 idle callback failed|M4 kernel RPC .*failed:|native DCP flip failed", kernels):
        return "display_fault"
    if re.search(r"compilation failed|link failed|major shader compiler error|no grounded AGX3 encoding|failed to compile or link required", stdout, re.I):
        return "shader_error"
    if re.search(r"(?:ERROR.*VUID-|SYNC-HAZARD-)", stdout, re.I):
        return "validation_error"
    if re.search(r"VK_ERROR_(?:DEVICE_LOST|FEATURE_NOT_PRESENT)|vkEndCommandBuffer failed|GL_INVALID_OPERATION|major api error|ZINK: blit unsupported", stdout, re.I):
        return "api_error"
    if re.search(r"\bbad=[1-9][0-9]*(?:/|\b)|^FAIL:|\bpixels(?: checked)?, [1-9][0-9]* failures$|^pixels=\d+ incorrect=[1-9]|\bpixel \d+ differs at byte \d+", stdout, re.M):
        return "readback_mismatch"
    result_path = directory / "remote" / "result.json"
    if not result_path.exists():
        return "incomplete"
    result = json.loads(result_path.read_text())
    watchdog = result.get("watchdog", {})
    thermal = result.get("thermal", {})
    if (result.get("boot_id") != facts["boot_id"] or
            watchdog.get("timeout_seconds") != 60 or not watchdog.get("armed") or
            thermal.get("failure") is not None or thermal.get("samples", 0) < 1):
        return "guard_validation_failed"
    # Host health evidence survives kernel-log overrun. A child can exit zero
    # before the guard's terminal health check; that must never become Pass.
    if result.get("progress_error") is not None:
        return "gpu_progress_failure"
    if result.get("returncode") != 0:
        return "process_failure"
    if not re.search(expected, stdout, re.M):
        return "missing_success_evidence"
    return "pass"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True)
    parser.add_argument("--mesa-source", type=pathlib.Path, default=ROOT / "mesa/m4-asahi",
                        help="Mesa checkout whose exact revision/patch is recorded")
    parser.add_argument("--cpu-max-khz", type=int, default=1860000, choices=[1860000, 4104000],
                        help="CPU ceiling the guard applies for the run (it never raises an existing lower cap)")
    parser.add_argument("--hypothesis", required=True)
    parser.add_argument("--expect", required=True, help="Required success regex in command.log or retained application.log")
    parser.add_argument("--seconds", type=int, default=40, choices=range(1, 1801), metavar="1..1800",
                        help="Runs over 40s require the qualified GPU-retirement watchdog guard")
    parser.add_argument("--thermal-stop-mc", type=int, choices=[95000, 100000, 120000], default=120000,
                        help="May tighten the installed independent thermal stop")
    parser.add_argument("--trace-kernel-writes",action="store_true",help="Retain failed kernel-log write syscalls")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="Remote argv; {output} expands to the evidence directory")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", args.name):
        parser.error("name must contain lowercase letters, digits and hyphens")
    re.compile(args.expect)
    argv = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not argv:
        parser.error("a remote command is required")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    run_id = stamp + "-" + args.name
    output = ROOT / "_provenance" / "experiments" / run_id
    output.mkdir(parents=True, exist_ok=False)
    remote_output = "/root/asahi-evidence/" + run_id
    argv = [arg.replace("{output}", remote_output) for arg in argv]
    metadata = {"id": run_id, "hypothesis": args.hypothesis, "command": argv,
                "expected_success": args.expect, "seconds": args.seconds, "thermal_stop_mc": args.thermal_stop_mc,
                "sources": {}}
    for name, repo in [("mesa", args.mesa_source), ("kernel", ROOT / "linux/linux-asahi")]:
        state, patch = source_state(repo)
        metadata["sources"][name] = state
        (output / (name + ".patch")).write_bytes(patch)
    write_json(output / "manifest.json", metadata)
    # Read-only identity and hashes before arming; no GPU command runs here.
    script = (ROOT / "tools/gpu-identity.py").read_text()
    metadata["identity_probe_sha256"] = hashlib.sha256(script.encode()).hexdigest()
    write_json(output / "manifest.json", metadata)
    facts = json.loads(subprocess.check_output(
        SSH + [shlex.join(["python3", "-c", script, *argv])], timeout=30))
    write_json(output / "target-before.json", facts)
    if args.seconds > 40:
        # The qualified guard requires verified GPU progress/idle CPU work AND live
        # thermal evidence. An older target helper must not silently weaken
        # the bounded experiment when a longer replay is requested.
        guard_hash = hashlib.sha256((ROOT / "tools/guarded-command.py").read_bytes()).hexdigest()
        if facts["files"].get("/root/guarded-command.py") != guard_hash:
            raise RuntimeError("Long experiment requires the matching qualified guarded-command.py")
        progress_hash = hashlib.sha256((ROOT / "tools/completion_progress.py").read_bytes()).hexdigest()
        if facts["files"].get("/root/completion_progress.py") != progress_hash:
            raise RuntimeError("Long experiment requires the matching completion_progress.py")
        idle_hash = hashlib.sha256((ROOT / "tools/idle_work_progress.py").read_bytes()).hexdigest()
        if facts["files"].get("/root/idle_work_progress.py") != idle_hash:
            raise RuntimeError("Long experiment requires the matching idle_work_progress.py")
    command = ["python3", "/root/guarded-command.py", "--seconds", str(args.seconds),
               "--cpu-max-khz", str(args.cpu_max_khz), "--thermal-stop-mc", str(args.thermal_stop_mc),
               "--output", remote_output, *(["--trace-kernel-writes"] if args.trace_kernel_writes else []), "--", *argv]
    print("Evidence:", output, flush=True)
    with (output / "guard.log").open("w") as log:
        try:
            completed = subprocess.run(SSH + [shlex.join(command)], stdout=log,
                                       stderr=subprocess.STDOUT, timeout=args.seconds + 75)
            transport = completed.returncode
        except subprocess.TimeoutExpired:
            transport = "timeout"
    # Watchdog/reboot recovery is left to the target guard. Never feed it here.
    fetched = False
    for attempt in range(7):
        with (output / "fetch.log").open("a") as log:
            try:
                fetched = subprocess.run(
                    ["rsync", "-az", "--partial", "--partial-dir=.rsync-partial",
                     "-e", "ssh -o ConnectTimeout=10", "--timeout=20",
                     HOST + ":" + remote_output + "/", str(output / "remote") + "/"],
                     stdout=log, stderr=subprocess.STDOUT, timeout=180).returncode == 0
            except subprocess.TimeoutExpired:
                log.write("Evidence fetch reached its 180-second transport bound; retrying with partial files retained.\n")
                fetched = False
        if fetched:
            break
        if attempt < 6:
            print("Retrying evidence transfer; partial files are retained.", flush=True)
            time.sleep(15)
    verdict = classify(output, facts, args.expect) if fetched else "evidence_unavailable"
    if verdict == "pass" and transport != 0:
        verdict = "guard_or_transport_failure"
    report = {"id": run_id, "verdict": verdict, "transport_returncode": transport,
              "evidence_fetched": fetched, "directory": str(output)}
    if fetched:
        report["observations"] = summarize_evidence(output)
    write_json(output / "report.json", report)
    print(json.dumps(report, indent=2), flush=True)
    return 0 if verdict == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
