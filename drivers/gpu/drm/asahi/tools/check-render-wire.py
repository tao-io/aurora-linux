#!/usr/bin/env python3
"""Compare a proposed M4 encoder with an immutable Git baseline, offline.

The same current test driver runs both encoder versions over varied owned
addresses, ASIDs, generations, counters, event slots and stamps. It is a byte
preservation check, not a firmware/hardware conformance oracle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--baseline', required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    relative = 'drivers/gpu/drm/asahi'
    driver = a.kernel / relative
    tree = subprocess.check_output(['git', '-C', str(a.kernel), 'rev-parse',
        '--verify', '--end-of-options', a.baseline + '^{tree}'], text=True).strip()
    versions = {}
    for version in ('baseline', 'candidate'):
        directory = a.output / version
        (directory / 'tools').mkdir(parents=True)
        hashes = {}
        for name in ('g16_render.rs', 'g16_compute.rs', 'g16_render_command.rs'):
            raw = (subprocess.check_output(['git', '-C', str(a.kernel), 'show', f'{tree}:{relative}/{name}'])
                   if version == 'baseline' else (driver / name).read_bytes())
            (directory / name).write_bytes(raw)
            hashes[name] = hashlib.sha256(raw).hexdigest()
        fixture = driver / 'tools/m4_render_wire_fixture.rs'
        shutil.copyfile(fixture, directory / 'tools/m4_render_wire_fixture.rs')
        hashes['fixture'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
        binary = directory / 'wire'
        subprocess.run(['rustc', '--edition=2021', str(directory / 'tools/m4_render_wire_fixture.rs'),
                        '-o', str(binary)], check=True)
        versions[version] = hashes
    comparisons = []
    for variant in (0, 1, 2, 15, 31, 63):
        outputs = []
        for version in versions:
            raw = subprocess.check_output([str((a.output / version / 'wire').resolve()), str(variant)])
            (a.output / f'{version}-{variant}.bin').write_bytes(raw)
            outputs.append(raw)
        if outputs[0] != outputs[1]:
            raise AssertionError(f'Wire mismatch in variant {variant}; both outputs retained')
        comparisons.append(dict(variant=variant, bytes=len(outputs[0]), sha256=hashlib.sha256(outputs[0]).hexdigest()))
    report = dict(result='pass', baseline_tree=tree, sources=versions, comparisons=comparisons,
                  rustc=subprocess.check_output(['rustc', '--version'], text=True).strip())
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(comparisons)} variants, both engines, identical wire bytes and rejected-input checks')


if __name__ == '__main__':
    main()
