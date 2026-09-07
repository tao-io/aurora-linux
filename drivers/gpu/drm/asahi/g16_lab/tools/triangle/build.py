#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the ordinary Vulkan/XCB triangle application and its SPIR-V shaders."""
import argparse
from pathlib import Path
import shlex
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
source = Path(__file__).resolve().parent
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
header = ['/* SPDX-License-Identifier: MIT */', '#include <stdint.h>']
for stage in ('vert', 'frag'):
    spirv = out / f'triangle.{stage}.spv'
    subprocess.run(['glslangValidator', '-V', str(source / f'triangle.{stage}'), '-o', str(spirv)], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(spirv)], check=True)
    data = spirv.read_bytes()
    words = struct.unpack(f'<{len(data)//4}I', data)
    header.append(f'static const uint32_t triangle_{stage}_spv[] = {{')
    header += ['  ' + ', '.join(f'0x{w:08x}' for w in words[i:i+8]) + ',' for i in range(0, len(words), 8)]
    header.append('};')
(out / 'triangle_spirv.h').write_text('\n'.join(header) + '\n')
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'xcb', 'vulkan'], text=True))
subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-I'+str(out),
                str(source / 'triangle.c'), '-o', str(out / 'm4-vulkan-triangle'), '-ldl', *flags], check=True)
print(out / 'm4-vulkan-triangle')
