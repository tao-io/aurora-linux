#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Resolve A000..A499 in one captured J713 DCP image, entirely offline.

Requires Unicorn. No firmware is included. Results are handler addresses relative
to __TEXT, not proof of RPC semantics or permission to issue hardware commands.
"""
import argparse
import hashlib
import json
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (
    UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_W23, UC_ARM64_REG_X24,
)

TEXT_SHA = "25b095cc293adb48c2c507a578b94810e5e06e7607eb13e5d64a6221e2c29793"
DATA_SHA = "a25153cea2ef90dbce9d14aa003950a68cd8acb2821dd2a99659bd2302b5ef59"
STOPS = (0x196ecc, 0x19732c, 0x196ed0)


def checked_image(path, expected):
    blob = path.read_bytes()
    if hashlib.sha256(blob).hexdigest() != expected:
        raise ValueError(f"{path}: unsupported firmware snapshot")
    return blob


def dispatch(text, data):
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for address, blob in ((0, text), (0x806000, data)):
        machine.mem_map(address, (len(blob) + 4095) & ~4095)
        machine.mem_write(address, blob)
    machine.mem_map(0x1000000, 0x10000)

    def instruction(uc, address, size, unused):
        if address in STOPS:
            uc.emu_stop()
            return
        word = int.from_bytes(uc.mem_read(address, 4), "little")
        # PACIZA signs the callback pointer; retain the unsigned address.
        if word & 0xffffffe0 == 0xdac123e0:
            uc.reg_write(UC_ARM64_REG_PC, address + 4)

    machine.hook_add(UC_HOOK_CODE, instruction)
    initial = machine.context_save()
    result = {}
    for number in range(500):
        tag = f"A{number:03d}"
        machine.context_restore(initial)
        machine.reg_write(UC_ARM64_REG_W23, int.from_bytes(tag.encode(), "big"))
        machine.reg_write(UC_ARM64_REG_SP, 0x1008000)
        # Enter after packet parsing, stop before callback invocation.
        machine.emu_start(0x196518, 0x197424, count=200)
        pc = machine.reg_read(UC_ARM64_REG_PC)
        if pc not in STOPS:
            raise RuntimeError(f"{tag}: unexpected dispatch exit {pc:#x}")
        handler = machine.reg_read(UC_ARM64_REG_X24)
        if handler:
            result[tag] = hex(handler)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("text", type=Path)
    parser.add_argument("data", type=Path)
    args = parser.parse_args()
    result = dispatch(checked_image(args.text, TEXT_SHA),
                      checked_image(args.data, DATA_SHA))
    print(json.dumps({"text_sha256": TEXT_SHA, "data_sha256": DATA_SHA,
                      "handlers": result}, indent=2))


if __name__ == "__main__":
    main()
