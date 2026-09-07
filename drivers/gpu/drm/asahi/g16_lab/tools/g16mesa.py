#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the bounded NIR probe against a configured, pinned Aurora Mesa tree."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

from g16agx3 import MESA_REVISION


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', required=True, type=Path)
    parser.add_argument('--mesa-build', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--spirv', nargs=2, type=Path, metavar=('VERTEX', 'FRAGMENT'))
    args = parser.parse_args()
    mesa, build, output = (p.resolve() for p in (args.mesa, args.mesa_build, args.output_dir))
    revision = subprocess.check_output(['git', '-C', str(mesa), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(mesa), 'status', '--porcelain', '--untracked-files=no'], text=True)
    if revision != MESA_REVISION or dirty:
        raise ValueError('requires the clean pinned Mesa revision')
    target = 'src/asahi/compiler/agx3_graphics_compile_test'
    subprocess.run(['ninja', '-C', str(build), '-j12', target, 'src/compiler/spirv/libvtn.a'], check=True)
    commands = json.loads((build / 'compile_commands.json').read_text())
    entry = next(e for e in commands if e['file'].endswith('/test-agx3-graphics-compile.c'))
    source = (Path(entry['directory']) / entry['file']).resolve()
    if source != mesa / 'src/asahi/compiler/test/test-agx3-graphics-compile.c':
        raise ValueError('configured build uses a different source checkout')
    output.mkdir(parents=True, exist_ok=True)
    probe = Path(__file__).with_suffix('.c').resolve()
    obj, exe = output / 'g16mesa.o', output / 'g16mesa'
    compile_args = shlex.split(entry['command'])
    compile_args = [str(probe) if a == entry['file'] else a.replace(entry['output'], str(obj))
                    for a in compile_args]
    subprocess.run(compile_args, cwd=build, check=True)
    link = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()[-1]
    link_args = shlex.split(link)
    if any(a in ('&&', ';', '|', '>') for a in link_args):
        raise ValueError('unexpected shell operations in the link command')
    link_args = [str(exe) if a == target else str(obj) if a == entry['output'] else a for a in link_args]
    link_args.insert(link_args.index('-Wl,--start-group') + 1, 'src/compiler/spirv/libvtn.a')
    link_args.insert(link_args.index('-Wl,--end-group'), '-lSPIRV-Tools')
    subprocess.run(link_args, cwd=build, check=True)
    run = subprocess.run([str(exe)] + [str(p.resolve()) for p in (args.spirv or [])], capture_output=True)
    (output / 'compiler.log').write_bytes(run.stderr)
    if run.returncode:
        print(run.stderr.decode(), end='')
        run.check_returncode()
    header = output / 'g16_mesa_graphics.h'
    header.write_bytes(run.stdout)
    (output / 'manifest.json').write_text(json.dumps({
        'mesa_revision': revision,
        'probe_sha256': hashlib.sha256(probe.read_bytes()).hexdigest(),
        'spirv_lowering_sha256': hashlib.sha256(probe.with_name('g16spirv.h').read_bytes()).hexdigest(),
        'header_sha256': hashlib.sha256(run.stdout).hexdigest(),
        'compiler_report': run.stderr.decode(),
        'compile_command': compile_args,
        'link_command': link_args,
        'spirv_inputs': [{"path": str(p.resolve()), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in (args.spirv or [])],
        'scope': 'offline SPIR-V compiler output with lab ABI lowering' if args.spirv else 'offline post-lowering NIR compiler output; hardware validation separate',
    }, indent=2) + '\n')
    print(run.stderr.decode(), end='')
    print(header)


if __name__ == '__main__':
    main()
