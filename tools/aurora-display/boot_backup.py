#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read boot inventory and optionally create a private, verified backup."""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import stat
import tarfile
from pathlib import Path


def quoted(value):
    return json.dumps(str(value), ensure_ascii=False)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inventory(boot):
    config = boot / "grub/grub.cfg"
    text = config.read_text() if config.is_file() else ""
    entries = re.findall(r"^\s*menuentry\s+(['\"])(.*?)\1", text, re.M)
    linux_lines = re.findall(r"^\s*linux\s+(\S+)", text, re.M)
    initrd_lines = re.findall(r"^\s*initrd\s+(.*)", text, re.M)
    boot_bins = sorted(str(p.relative_to(boot)) for p in boot.rglob("boot.bin"))
    return {
        "menu_titles": [title for _, title in entries],
        "linux_paths": linux_lines,
        "initrd_paths": initrd_lines,
        "boot_bin_paths": boot_bins,
        "grub_config_present": config.is_file(),
        "grub_config_sha256": digest(config) if config.is_file() else "",
    }


def snapshot(boot, destination):
    """Copy regular files and symlinks; verify every copied file."""
    if destination.exists():
        raise ValueError("Backup destination already exists")
    records = []
    for path in sorted(boot.rglob("*")):
        mode = path.lstat().st_mode
        relative = str(path.relative_to(boot))
        if stat.S_ISREG(mode):
            records.append((relative, "file", digest(path)))
        elif stat.S_ISLNK(mode):
            records.append((relative, "link", os.readlink(path)))
        elif not stat.S_ISDIR(mode):
            raise ValueError("Unexpected special file in boot tree")
    if not records:
        raise ValueError("Boot tree is empty")
    destination.mkdir(mode=0o700)
    archive = destination / "boot.tar"
    with tarfile.open(archive, "w", dereference=False) as bundle:
        bundle.add(boot, arcname="boot")
    archive.chmod(0o600)
    with tarfile.open(archive, "r") as bundle:
        for relative, kind, expected in records:
            name = "boot/" + relative
            item = bundle.getmember(name)
            if kind == "file":
                with bundle.extractfile(item) as stream:
                    actual = hashlib.file_digest(stream, "sha256").hexdigest()
                if actual != expected or digest(boot / relative) != expected:
                    raise ValueError("Boot file changed during backup: " + relative)
            elif not item.issym() or item.linkname != expected:
                raise ValueError("Backup symlink differs: " + relative)
    manifest = "schema_version = 1\nverified_archive = true\nrollback_tested = false\n"
    manifest += "archive_sha256 = " + quoted(digest(archive)) + "\n"
    for relative, kind, value in records:
        manifest += "\n[[entries]]\npath = " + quoted(relative) + "\n"
        manifest += "kind = " + quoted(kind) + "\nvalue = " + quoted(value) + "\n"
    receipt = destination / "manifest.toml"
    receipt.write_text(manifest)
    receipt.chmod(0o600)
    return archive, len(records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backup", action="store_true")
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("Run this command with sudo on the Air")
    model = Path("/proc/device-tree/model").read_bytes().rstrip(b"\0")
    compatible = Path("/proc/device-tree/compatible").read_bytes().split(b"\0")
    if b"apple,j313" not in compatible or b"apple,t8103" not in compatible:
        parser.error("This helper is limited to the J313 M1 Air")
    boot = Path("/boot")
    if not os.path.ismount(boot) or not os.path.ismount(boot / "efi"):
        parser.error("Both /boot and /boot/efi must already be mounted")
    os.umask(0o077)
    data = inventory(boot)
    print("schema_version = 1")
    print("model = " + quoted(model.decode()))
    print("running_kernel = " + quoted(platform.release()))
    print("boot_files_changed = false")
    print("rollback_tested = false")
    for key, value in data.items():
        if isinstance(value, bool):
            print(key + " = " + str(value).lower())
        elif isinstance(value, list):
            print(key + " = [" + ", ".join(quoted(v) for v in value) + "]")
        else:
            print(key + " = " + quoted(value))
    if args.backup:
        parent = Path("/var/lib/aurora-display/backups")
        for path in [parent.parent, parent]:
            if path.is_symlink():
                parser.error("Backup directory must not be a symlink")
            path.mkdir(mode=0o700, parents=True, exist_ok=True)
            if path.stat().st_uid != 0:
                parser.error("Backup directory must be owned by root")
            path.chmod(0o700)
        stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%S.%fZ")
        archive, count = snapshot(boot, parent / stamp)
        print("backup_archive = " + quoted(archive))
        print("backup_archive_sha256 = " + quoted(digest(archive)))
        print("backup_entries = " + str(count))
        print("backup_verified = true")


if __name__ == "__main__":
    main()
