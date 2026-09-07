#!/usr/bin/env python3
"""Generate a standalone G16 Mandelbrot compute shader without Metal.

The first compiler target intentionally emits straight-line code. Iterations
are unrolled so the shader only relies on encodings already cross-checked
between captured G16 programs and the public M3 encoder.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
VENDOR = ROOT / "vendor" / "applegpu_m3"
sys.path.insert(0, str(VENDOR))

import assemble  # type: ignore  # noqa: E402


def float_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def source(size: int = 64, iterations: int = 96) -> str:
    if size < 2 or size > 256 or size & (size - 1):
        raise ValueError("size must be a power of two between 2 and 256")
    if iterations < 1:
        raise ValueError("iterations must be positive")

    shift = int(math.log2(size))
    scale = float_bits(3.0 / (size - 1))
    lines = [
        "; G16 compute ABI: u4:u5 = device uint *out, sr32 = linear ID",
        f"; output is a {size}x{size} row-major RGBA8 Mandelbrot image",
        "get_sr 0, r1, sr32",
        f"bfeil r2, 0, r1, {shift}",
        "convert u32_to_f, r2, r2, rte",
        f"mov_imm r3, 0x{scale:08x}",
        "ffma r2, r2, r3, -1.5",
        f"and r4, r1, {size - 1}",
        "convert u32_to_f, r4, r4, rte",
        "ffma r4, r4, r3, -2.0",
        "mov_imm r5, 0",
        "mov_imm r6, 0",
    ]

    # r3 held the scale only during coordinate setup, so it can be the second
    # z-real register. Keeping the whole program at r8 or below matches the
    # highest GPR already exercised by the verified context-5 iadd body.
    zr = "r5"
    for iteration in range(iterations):
        next_zr = "r3" if zr == "r5" else "r5"
        lines.extend(
            [
                f"; iteration {iteration}",
                "fmul r8, r6, r6",
                f"ffma r7, {zr}, {zr}, r8.neg",
                f"fadd {next_zr}, r7, r4",
                f"fmul r8, 2.0, {zr}",
                "ffma r6, r8, r6, r2",
            ]
        )
        zr = next_zr

    lines.extend(
        [
            f"fmul r7, {zr}, {zr}",
            "ffma r7, r6, r6, r7",
            "; ci/cr are dead now; reuse their registers for packed colors",
            "mov_imm r2, 0xff000000",
            "mov_imm r4, 0xffffffff",
            "; black inside (|z| <= 2), white outside",
            "cmpsel fle, r8, r7, 4.0, r2, r4",
            "device_store 0, i32, x, r8, u4_u5, r1, lsl 2, 0, 1, 1",
            "stop",
        ]
    )
    return "\n".join(lines) + "\n"


def assemble_g16(text: str) -> bytes:
    code = bytes(assemble.assemble_multiline(text.splitlines()))
    # The public assembler pads the two-byte stop to four bytes. Captured G16
    # executable payloads terminate at the 0x000e halfword itself.
    if not code.endswith(bytes.fromhex("0e000000")):
        raise ValueError("unexpected stop encoding")
    return code[:-2]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--assembly", type=Path)
    parser.add_argument("--size", type=int, default=64)
    parser.add_argument("--iterations", type=int, default=96)
    args = parser.parse_args()

    text = source(args.size, args.iterations)
    code = assemble_g16(text)
    args.output.write_bytes(code)
    if args.assembly:
        args.assembly.write_text(text)
    print(
        f"wrote 0x{len(code):x} bytes: {args.size}x{args.size}, "
        f"{args.iterations} iterations"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
