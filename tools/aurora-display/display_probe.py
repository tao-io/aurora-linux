#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read LG display state. A software pass is not proof of panel pixels."""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path


def toml_value(value):
    if isinstance(value, bool):
        return str(value).lower()
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, list):
        return "[" + ", ".join(toml_value(v) for v in value) + "]"
    if isinstance(value, dict):
        return "{ " + ", ".join(k + " = " + toml_value(v) for k, v in value.items()) + " }"
    return json.dumps(str(value), ensure_ascii=False)


def emit(report):
    lines = [name + " = " + toml_value(value) for name, value in report.items()
             if not isinstance(value, dict)
             and not (isinstance(value, list) and value and isinstance(value[0], dict))]
    for name, value in report.items():
        if isinstance(value, dict):
            lines.append("\n[" + name + "]")
            lines.extend(k + " = " + toml_value(v) for k, v in value.items())
        elif isinstance(value, list) and value and isinstance(value[0], dict):
            for item in value:
                lines.append("\n[[" + name + "]]")
                lines.extend(k + " = " + toml_value(v) for k, v in item.items())
    return "\n".join(lines) + "\n"


def run(argv, env=None):
    try:
        result = subprocess.run(argv, text=True, capture_output=True, env=env, timeout=20)
        return result.returncode, result.stdout, result.stderr.strip()[:300]
    except (OSError, subprocess.TimeoutExpired) as error:
        return -1, "", str(error)[:300]


def read(path, binary=False):
    try:
        return path.read_bytes() if binary else path.read_text().strip().rstrip("\0")
    except OSError:
        return b"" if binary else ""


def edid_info(data):
    info = {"bytes": len(data), "valid": False, "manufacturer": "", "name": ""}
    if not data:
        return info
    info["sha256"] = hashlib.sha256(data).hexdigest()
    if len(data) < 128 or len(data) % 128 or data[:8] != b"\x00\xff\xff\xff\xff\xff\xff\x00":
        return info
    manufacturer = int.from_bytes(data[8:10], "big")
    info["manufacturer"] = "".join(chr(64 + ((manufacturer >> shift) & 31)) for shift in (10, 5, 0))
    info["product"] = int.from_bytes(data[10:12], "little")
    info["extension_count"] = data[126]
    info["valid"] = len(data) >= 128 * (1 + data[126]) and all(sum(data[i:i+128]) % 256 == 0 for i in range(0, len(data), 128))
    for offset in (54, 72, 90, 108):
        block = data[offset:offset+18]
        if block[:5] == b"\x00\x00\x00\xfc\x00":
            info["name"] = block[5:].decode("ascii", "replace").strip("\0\n ")
    return info


def collect():
    errors = []
    tb = []
    for path in sorted(Path("/sys/bus/thunderbolt/devices").glob("*")):
        name = read(path / "device_name")
        if not name:
            continue
        tb.append({"route": path.name, "name": name,
                   "vendor": read(path / "vendor_name"),
                   "authorized": read(path / "authorized") == "1",
                   "rx_speed": read(path / "rx_speed"), "rx_lanes": read(path / "rx_lanes"),
                   "tx_speed": read(path / "tx_speed"), "tx_lanes": read(path / "tx_lanes")})
    mode_map = {}
    code, output, error = run(["drm_info", "-j"])
    if code == 0:
        try:
            for device, details in json.loads(output).items():
                for connector in details.get("connectors", []):
                    mode_map[(Path(device).name, connector["id"])] = connector
        except (ValueError, KeyError, AttributeError, TypeError) as error:
            errors.append("drm_info parse: " + str(error))
    else:
        errors.append("drm_info: " + error)
    drm = []
    for path in sorted(Path("/sys/class/drm").glob("card*-*")):
        if not (path / "status").exists():
            continue
        card, name = path.name.split("-", 1)
        connector_id = read(path / "connector_id")
        details = mode_map.get((card, int(connector_id)), {}) if connector_id.isdigit() else {}
        modes = [{"width": m.get("hdisplay", 0), "height": m.get("vdisplay", 0),
                  "refresh": m.get("vrefresh", 0), "flags": m.get("flags", 0),
                  "preferred": bool(m.get("type", 0) & 8)} for m in details.get("modes", [])]
        properties = details.get("properties", {})
        drm.append({"device": card, "name": name, "id": int(connector_id) if connector_id.isdigit() else -1,
                    "status": read(path / "status"), "enabled": read(path / "enabled"),
                    "edid": edid_info(read(path / "edid", binary=True)), "modes": modes,
                    "link_status": properties.get("link-status", {}).get("value", -1),
                    "crtc_id": properties.get("CRTC_ID", {}).get("value", -1)})
    env = dict(os.environ, XDG_RUNTIME_DIR=f"/run/user/{os.getuid()}")
    hyprland = []
    code, output, error = run(["hyprctl", "instances", "-j"], env)
    if code == 0:
        try:
            instances = json.loads(output)
            if len(instances) != 1:
                raise ValueError("Expected one Hyprland instance")
            env["HYPRLAND_INSTANCE_SIGNATURE"] = instances[0]["instance"]
            code, output, error = run(["hyprctl", "monitors", "all", "-j"], env)
            if code:
                raise ValueError(error)
            for monitor in json.loads(output):
                allowed = ("name", "description", "make", "model", "width", "height", "refreshRate", "scale", "disabled", "dpmsStatus", "transform")
                hyprland.append({k: v for k, v in monitor.items() if k in allowed})
        except (ValueError, KeyError, TypeError) as error:
            errors.append("hyprctl: " + str(error))
    else:
        errors.append("hyprctl: " + error)
    code, output, error = run(["journalctl", "-b", "-k", "--since", "5 minutes ago", "--no-pager", "-o", "short-monotonic"])
    pattern = re.compile(r"thunderbolt|DPTX|dpin|tiled:|crossbar|ACIO|tunneled native|PCIe-C|flip_done|DCP.*(?:mode|fault|error)", re.I)
    logs = [line for line in output.splitlines() if pattern.search(line)][-120:] if code == 0 else []
    if code:
        errors.append("journalctl: " + error)
    return {"machine": {"kernel": platform.release(), "model": read(Path("/proc/device-tree/model")),
                        "m1n1_stage2": read(Path("/proc/device-tree/chosen/asahi,m1n1-stage2-version")).rstrip("\0")},
            "thunderbolt": tb, "drm": drm, "hyprland": hyprland, "recent_display_logs": logs, "errors": errors}


def evaluate(report, width=5120, height=2880, refresh=60.0):
    checks = []
    def add(name, passed, reason):
        checks.append({"name": name, "status": "pass" if passed else "fail", "reason": reason})
    authorized = [d for d in report["thunderbolt"]
                  if ("lg" in d["name"].lower() or d.get("vendor", "").lower().startswith("lg "))
                  and d["authorized"]]
    add("lg_thunderbolt", len(authorized) == 1, "Exactly one authorized LG Thunderbolt device")
    candidates = [d for d in report["drm"] if d["status"] == "connected" and d["edid"]["valid"]
                  and (d["edid"]["manufacturer"] == "GSM" or "lg" in d["edid"]["name"].lower())]
    add("lg_edid", len(candidates) == 1, "Exactly one connected LG connector with a complete, valid EDID")
    native = []
    for connector in candidates:
        modes = [m for m in connector["modes"] if m["width"] == width and m["height"] == height
                 and abs(m["refresh"] - refresh) <= 0.5 and not m["flags"] & (16 | 32)]
        if modes:
            native.append(connector)
    add("drm_native_mode", len(native) == 1, "One LG connector offers progressive 5120x2880 near 60 Hz")
    active = []
    for connector in native:
        for monitor in report["hyprland"]:
            if monitor["name"] != connector["name"]:
                continue
            if (monitor.get("width") == width and monitor.get("height") == height
                    and abs(monitor.get("refreshRate", 0) - refresh) <= 0.5
                    and monitor.get("disabled") is False and monitor.get("dpmsStatus") is True
                    and connector["enabled"] == "enabled" and connector["link_status"] == 0):
                active.append(monitor)
    add("active_lg_mode", len(active) == 1, "The same LG output is enabled at native 5K60 in DRM and Hyprland")
    checks.extend([{"name": "live_tile_routing", "status": "unknown", "reason": "Requires a live DCP routing readout; old log lines are not current state"},
                   {"name": "physical_panel", "status": "unknown", "reason": "Needs a panel test pattern observed by a person or a provided camera"}])
    report["checks"] = checks
    report["result"] = {"software_mode_pass": all(c["status"] == "pass" for c in checks[:4]),
                        "support_verified": False, "panel_observed": False}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-mode", action="store_true", help="Return 1 unless software 5K60 checks pass")
    args = parser.parse_args()
    report = evaluate(collect())
    report = {"schema_version": 1, "recorded_at_utc": datetime.datetime.now(datetime.UTC).isoformat(), **report}
    sys.stdout.write(emit(report))
    return 1 if args.require_mode and not report["result"]["software_mode_pass"] else 0


if __name__ == "__main__":
    sys.exit(main())
