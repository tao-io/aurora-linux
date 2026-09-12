#!/usr/bin/env python3
"""Disassemble a bounded range of a saved ARM64 firmware image, with provenance.

Offline only: this does not connect to hardware or read live memory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def integer(value):
    return int(value, 0)


p = argparse.ArgumentParser(description=__doc__)
p.add_argument("image", type=Path)
p.add_argument("--offset", required=True, type=integer)
p.add_argument("--size", required=True, type=integer)
p.add_argument("--output", required=True, type=Path)
p.add_argument("--objdump", default="aarch64-linux-gnu-objdump")
a = p.parse_args()
data = a.image.read_bytes()
if a.offset < 0 or a.size <= 0 or (a.offset | a.size) & 3:
    p.error("offset and size must describe a positive, instruction-aligned range")
if a.offset + a.size > len(data):
    p.error("range exceeds the saved image")
command = [a.objdump, "-D", "-b", "binary", "-m", "aarch64",
           f"--start-address={a.offset}", f"--stop-address={a.offset + a.size}",
           str(a.image.resolve())]
result = subprocess.run(command, check=True, capture_output=True, text=True)
a.output.parent.mkdir(parents=True, exist_ok=True)
with a.output.open("x") as output:
    output.write(result.stdout)
metadata = dict(image=str(a.image.resolve()), size=len(data),
                sha256=hashlib.sha256(data).hexdigest(), offset=a.offset,
                length=a.size, command=command)
with a.output.with_suffix(a.output.suffix + ".json").open("x") as output:
    json.dump(metadata, output, indent=2)
    output.write("\n")
print(a.output)
