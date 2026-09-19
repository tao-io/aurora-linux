#!/usr/bin/env python3
"""Guarded current Factorio capture with exact pass timings and pixel comparison.

Requires kernel SubmitTiming + SubmitSummary (debug_flags=25165824). Uses only
the experiment's live kernel log: whole-boot logs can contain identical reused
control-stream addresses from previous clients. Does not install a driver.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--name', required=True)
p.add_argument('--hypothesis', required=True)
p.add_argument('--capture-shaders', action='store_true')
p.add_argument('--render-count', type=int, default=10, help='Expected passes per frame; non-default is an adapted diagnostic, not a timing baseline')
p.add_argument('--icd', default='/opt/asahi-m4-port/asahi_icd.json')
p.add_argument('--env', action='append', default=[])
p.add_argument('--query-reference', type=Path, default=ROOT / '_provenance/experiments/'
    '20260919T142127135895Z-factorio-parameter-engines-early/remote/command.log',
    help='Exact captured query results to compare when HK_M4_COUNT_FRAGMENTS is enabled')
p.add_argument('--memory-floor', type=int, choices=range(6))
p.add_argument('--fabric-floor', type=int, choices=range(4))
p.add_argument('--gfx-voter', action='store_true')
p.add_argument('--ap-floor', action='store_true')
p.add_argument('--cache-quota-group', type=int, choices=(18,24))
p.add_argument('--reference', type=Path, default=ROOT / '_provenance/experiments/'
    '20260919T080918619848Z-factorio-mcc-cache-replay/remote/vulkan-replay/screenshot_frame_5.png')
a = p.parse_args()
env = dict.fromkeys(('HK_ARCHITECTURE_PROFILE', 'HK_M4_SHADER_PROFILE',
    'MESA_SHADER_CACHE_DISABLE', 'HK_M4_COMPUTE_TIMESTAMP_COPIES',
    'HK_M4_BATCH_QUERY_COPIES', 'HK_M4_SPECIALIZE_QUERY_COPY',
    'HK_M4_NATIVE_PREAMBLE', 'AGX_NATIVE_FLOAT_PREAMBLE',
    'HK_M4_PREFETCH_DESCRIPTORS', 'HK_M4_BATCH_DISJOINT_CLEARS',
    'HK_M4_COMPRESSED_2D', 'HK_M4_COMPRESSED_FEEDBACK',
    'HK_M4_COMPUTE_COMPRESSED_COPY', 'HK_M4_COMPRESSION_AUDIT'), 'true')
for entry in a.env:
    key, sep, value = entry.partition('=')
    if not sep or not re.fullmatch('[A-Z][A-Z0-9_]*', key):
        p.error('environment must be NAME=value')
    env[key] = value
floor_command = []
if a.memory_floor is not None or a.fabric_floor is not None:
    if a.memory_floor is None or a.fabric_floor is None or not (a.memory_floor or a.fabric_floor):
        p.error('supply both nonzero-in-total native floor selectors')
    env.update(M4_MEMORY_FLOOR_GUARDED_PROBE='1', M4_ENABLE_CLPC_VOTER='1')
    floor_command = ['/root/m4-memory-floor-probe', str(a.memory_floor), str(a.fabric_floor)]
if a.gfx_voter:
    if floor_command:
        p.error('GFX voter must be tested separately from CLPC floors')
    env.update(M4_MEMORY_FLOOR_GUARDED_PROBE='1', M4_ENABLE_GFX_VOTER='1')
    floor_command = ['/root/m4-memory-floor-probe', '0', '0']
if a.ap_floor:
    if not floor_command or a.gfx_voter:
        p.error('AP slot needs memory/fabric floors and excludes GFX-only mode')
    env.pop('M4_ENABLE_CLPC_VOTER', None)
    env['M4_MEMORY_FLOOR_AP'] = '1'
if a.cache_quota_group is not None:
    if floor_command:
        p.error('cache quota and memory floor experiments must run separately')
    env['M4_QUOTA_GUARDED_PROBE'] = '1'
    floor_command = ['/root/m4-mcc-quota-probe', str(a.cache_quota_group)]
before = set((ROOT / '_provenance/experiments').glob('*-' + a.name))
command = [sys.executable, str(ROOT / 'tools/gpu-experiment.py'),
    '--name', a.name, '--hypothesis', a.hypothesis,
    '--expect', 'PASS: Vulkan replay process completed', '--seconds', '65',
    '--cpu-max-khz', '4104000', '--', 'env',
    *[f'{k}={v}' for k,v in env.items()],
    *floor_command, 'python3', '/root/factorio-replay.py', '{output}', '--compression',
    '--icd', a.icd, '--trace', '/home/e/.local/share/factorio-driver/'
    'frame-architecture-20260919/menu_frames_1500_through_1504.gfxr', '--frames', '5']
if a.capture_shaders:
    command += ['--capture-shaders', '--capture-min-instructions', '0']
result = subprocess.run(command, cwd=ROOT)
if result.returncode:
    raise SystemExit(result.returncode)
created = set((ROOT / '_provenance/experiments').glob('*-' + a.name)) - before
assert len(created) == 1, created
directory = created.pop()
assert json.loads((directory / 'report.json').read_text())['verdict'] == 'pass'
kernel = (directory / 'remote/kernel-live.log').read_text()
mesa = (directory / 'remote/command.log').read_text()
if a.cache_quota_group is not None:
    assert 'PASS: native MCC quota request and restore' in mesa
    assert 'MCC_QUOTA restored=8 all-groups-unchanged=PASS' in mesa
elif floor_command:
    assert 'PASS: native memory floor request and restore' in mesa
    assert 'MEMORY_FLOOR restored=0' in mesa
spec = importlib.util.spec_from_file_location('cost', ROOT / 'tools/gpu-frame-cost.py')
cost = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cost)
candidates = []
for asid in sorted(set(map(int, re.findall(r'G16G_PASS .*?asid=(\d+)', kernel)))):
    report = cost.summarize(kernel, mesa, asid)
    frames = [row for row in report['submissions'] if row['render_count'] == a.render_count]
    if len(frames) == 5 and not report['unmatched']:
        candidates.append(report)
assert len(candidates) == 1, 'Missing or ambiguous complete five-frame GPU evidence'
report = candidates[0]
reference = Image.open(a.reference).convert('RGBA')
actual = Image.open(directory / 'remote/vulkan-replay/screenshot_frame_5.png').convert('RGBA')
report.update(evidence=str(directory), kernel_log='remote/kernel-live.log', expected_render_count=a.render_count, adapted_diagnostic=a.render_count != 10 or any(
        env.get(k, "false").lower() in ("true", "1") for k in (
            "HK_M4_COUNT_FRAGMENTS", "HK_M4_SKIP_FRAGMENT_STATISTICS_DIAGNOSTIC")),
    reference=str(a.reference), pixels_equal=reference.size == actual.size and
        reference.tobytes() == actual.tobytes())
if env.get('HK_M4_COUNT_FRAGMENTS', 'false').lower() in ('true', '1'):
    def query_results(log):
        return [tuple(map(int, row)) for row in re.findall(
            r'HK_FRAGMENT_QUERY pool=[0-9a-f]+ query=(\d+) covered=(\d+) available=(\d+)', log)]
    expected = query_results(a.query_reference.read_text())
    actual_queries = query_results(mesa)
    assert expected, 'Query reference contains no results'
    report.update(query_reference=str(a.query_reference),
        query_results=actual_queries, query_results_equal=actual_queries == expected)
(directory / 'frame-cost.json').write_text(json.dumps(report, indent=2) + '\n')
if 'query_results_equal' in report:
    assert report['query_results_equal'], 'Query totals differ: retain evidence before accepting'

for row in report['submissions']:
    if row['render_count'] == a.render_count:
        print('timeline', row['timeline'], 'fragment_ms', row['fragment_ms'],
            'render_span_ms', row['first_render_to_last_render_ms'],
            'whole_gpu_ms', row['whole_submission_gpu_ms'])
assert report['pixels_equal'], 'Frame differs: retain evidence and inspect before accepting'
print('PASS: five matched frames and pixel-exact final image' +
      ('; exact query totals' if report.get('query_results_equal') else ''))
