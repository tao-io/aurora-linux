#!/usr/bin/env python3
"""Join opt-in HK_ARCHITECTURE_PROFILE/HK_M4_SHADER_PROFILE to G16G_PASS.

One replay process per shader log. Select its ASID explicitly. These are GPU
pass spans, not wall-clock frame times or utilization. Unmatched lifetimes
are reported and excluded, never guessed from neighboring log lines.
"""
import argparse
import collections
import json
from pathlib import Path
import re


def fields(line):
    return dict(re.findall(r'(\w+)=([^ ]+)', line.strip()))


def summarize(kernel, mesa, asid):
    recorded = collections.defaultdict(list)
    pending = None
    returns = {}
    expected_commands = {}
    for line in mesa.splitlines():
        if 'HK_ARCH_SUBMIT ' in line:
            row = fields(line.split('HK_ARCH_SUBMIT ', 1)[1])
            expected_commands[(row['queue'], row['timeline'])] = int(row['commands'])
        if line.startswith('HK_ARCH_CS '):
            row = fields(line)
            pending = row if row['engine'] == 'render' else None
        elif line.startswith('HK_SHADER_SUBMIT ') and pending is not None:
            recorded[int(fields(line)['cs'], 16)].append(pending)
            pending = None
        elif line.startswith('HK_ARCH_RETURN '):
            row = fields(line)
            returns[(row['queue'], row['timeline'])] = int(row['elapsed_ns'])
    retired = collections.defaultdict(list)
    compute = collections.defaultdict(list)
    pending_compute = None
    for line in kernel.splitlines():
        if 'G16G: compute completion status=[' in line:
            words = line.split('status=[', 1)[1].split(']', 1)[0].split(',')
            pending_compute = [int(word.strip(), 16) for word in words]
        elif 'G16G_TIMING command ' in line:
            row = {k:int(v) for k,v in fields(line.split('G16G_TIMING command ', 1)[1]).items()}
            if row['pipe'] == 2 and pending_compute is not None:
                start, end = pending_compute[2:4]
                if (start == 0 and end == 0) or (start and end >= start):
                    compute[row['job']].append(dict(job=row['job'], index=row['index'],
                        gpu_start=start, gpu_end=end))
            pending_compute = None
        if 'G16G_PASS ' not in line:
            continue
        row = {k:int(v, 16 if v.startswith('0x') else 10)
               for k,v in fields(line.split('G16G_PASS ', 1)[1]).items()}
        if row['asid'] == asid:
            retired[row['cs']].append(row)
    submissions = {}
    unmatched = []
    for cs, rows in recorded.items():
        matches = retired.get(cs, [])
        if len(rows) != len(matches):
            unmatched.append(dict(cs=hex(cs), recorded=len(rows), retired=len(matches)))
            continue
        for meta, gpu in zip(rows, matches):
            key = (meta['queue'], meta['timeline'])
            dest = submissions.setdefault(key, dict(queue=int(key[0]), timeline=int(key[1]),
                submit_cpu_ms=returns.get(key, 0)/1e6,
                expected_commands=expected_commands.get(key), passes=[]))
            dest['passes'].append(dict(cb=int(meta['cb']), draws=int(meta['draws']),
                empty_tiles=int(meta['empty_tiles']), sample_bytes=int(meta['sample_bytes']),
                **gpu))
    for sub in submissions.values():
        sub['passes'].sort(key=lambda r:(r['job'], r['index']))
        sub['render_count'] = len(sub['passes'])
        sub['ta_ms'] = sum(r['ta_ns'] for r in sub['passes'])/1e6
        sub['fragment_ms'] = sum(r['fs_ns'] for r in sub['passes'])/1e6
        sub['ta_to_fragment_ms'] = sum(r['gap_ns'] for r in sub['passes'])/1e6
        sub['first_render_to_last_render_ms'] = ((
            max(r['gpu_end'] for r in sub['passes']) - min(r['gpu_start'] for r in sub['passes']))/24000
            if all(r['gpu_start'] and r['gpu_end'] >= r['gpu_start'] for r in sub['passes']) else None)
        sub['compute'] = [c for job in sorted({r['job'] for r in sub['passes']}) for c in compute[job]]
        commands = sub['passes'] + sub['compute']
        sub['complete_command_timestamps'] = (len(commands) == sub['expected_commands'] and
            len({(r['job'], r['index']) for r in commands}) == len(commands) and
            all(r['gpu_start'] and r['gpu_end'] >= r['gpu_start'] for r in commands))
        sub['whole_submission_gpu_ms'] = ((max(r['gpu_end'] for r in commands) -
            min(r['gpu_start'] for r in commands))/24000
            if sub['complete_command_timestamps'] else None)
        # Minimal profiling retains the first and last command of each packet.
        # Those brackets still measure the WHOLE packet, not sampled stages.
        ordered = sorted(commands, key=lambda r:(r['job'], r['index']))
        bracketed = (len({r['job'] for r in commands}) == 1 and ordered and
            ordered[0]['index'] == 0 and
            ordered[-1]['index'] + 1 == sub['expected_commands'] and
            ordered[0]['gpu_start'] > 0 and
            ordered[-1]['gpu_end'] >= ordered[0]['gpu_start'])
        sub['packet_boundary_timestamps'] = bool(bracketed)
        if not sub['complete_command_timestamps']:
            sub['ta_ms'] = sub['fragment_ms'] = sub['ta_to_fragment_ms'] = None
            if bracketed:
                sub['whole_submission_gpu_ms'] = (ordered[-1]['gpu_end'] - ordered[0]['gpu_start']) / 24000
        if sub['complete_command_timestamps']:
            # TA and fragment timestamps enclose engine stages separately.
            # Merge their intervals with compute, rather than double-counting
            # overlap or treating a TA->fragment dependency wait as execution.
            # Outside these intervals is not proven hardware idle: firmware,
            # cache operations and other clients are outside this accounting.
            intervals = [(r['gpu_start'] / 24000, r['gpu_end'] / 24000)
                         for r in sub['compute']]
            for r in sub['passes']:
                intervals += [(r['gpu_start'] / 24000,
                               r['gpu_start'] / 24000 + r['ta_ns'] / 1e6),
                              (r['gpu_end'] / 24000 - r['fs_ns'] / 1e6,
                               r['gpu_end'] / 24000)]
            merged = []
            for start, end in sorted(intervals):
                if merged and start <= merged[-1][1]:
                    merged[-1][1] = max(end, merged[-1][1])
                else:
                    merged.append([start, end])
            sub['instrumented_engine_union_ms'] = sum(end-start for start,end in merged)
            sub['outside_instrumented_engine_ms'] = (
                sub['whole_submission_gpu_ms'] - sub['instrumented_engine_union_ms'])
    return dict(asid=asid, scope='Exact render pass lifetimes in one Vulkan submission. Spans between render passes include compute, queue gaps and other clients; not pure GPU execution or live FPS.',
        unmatched=unmatched, submissions=sorted(submissions.values(), key=lambda r:r['timeline']))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernel', type=Path)
    p.add_argument('mesa', type=Path)
    p.add_argument('--asid', type=int, required=True)
    a = p.parse_args()
    print(json.dumps(summarize(a.kernel.read_text(), a.mesa.read_text(), a.asid), indent=2))
