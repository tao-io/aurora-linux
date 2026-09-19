#!/usr/bin/env python3
"""Summarize SubmitSummary retirement intervals across all GPU clients.

Use --tail-seconds to exclude initialization. GPU spans include TA-to-fragment
latency; active spans include firmware pickup and host observation. Neither is
a hardware utilization counter. Only fully contained intervals are selected.
"""
import argparse
import json
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path)
p.add_argument('--tail-seconds', type=float)
a = p.parse_args()
lines = a.log.read_text().splitlines()
rows = []
for line in lines:
    if 'userspace GPU job' not in line or 'summary_span_ns=' not in line:
        continue
    rows.append({k:int(v) for k,v in re.findall(r'\b([a-z_]+)=(-?\d+)', line)})
if not rows:
    p.error('No SubmitSummary intervals')
end = rows[-1]['monotonic_ns']
if a.tail_seconds is not None:
    start = end - a.tail_seconds * 1e9
    rows = [r for r in rows if r['monotonic_ns'] - r['summary_span_ns'] >= start]
if not rows:
    p.error('No complete intervals in requested window')
sums = {k:sum(r[k] for r in rows) for k in rows[0] if k != 'monotonic_ns'}
result = dict(intervals=len(rows), span_seconds=sums['summary_span_ns']/1e9,
              scope='All clients; timestamp spans, not hardware utilization', engines={})
for engine in ['render','compute']:
    n=sums[engine+'_commands']
    d=dict(commands=n)
    for name in ['prepare','active','gpu']:
        ns=sums[f'{engine}_{name}_ns']
        d[name+'_seconds']=ns/1e9
        d[name+'_mean_ms']=ns/1e6/n if n else None
    result['engines'][engine]=d
if 'render_private_clear_bytes' in sums:
    n = sums['render_commands']
    result['render_clears'] = dict(
        private_bytes=sums['render_private_clear_bytes'],
        private_mean_bytes=sums['render_private_clear_bytes']/n if n else None,
        private_seconds=sums['render_private_clear_ns']/1e9,
        total_seconds=sums['render_clear_ns']/1e9)
if 'render_tiling_ns' in sums:
    result['render_stages_seconds'] = {name:sums['render_'+name+'_ns']/1e9
                                       for name in ['tiling','stage_gap','fragment']}
selected = {r['monotonic_ns'] for r in rows}
geometry = {}
waits = []
for line in lines:
    if 'render geometry summary' not in line and 'completion wait totals' not in line:
        continue
    r = {k:int(v) for k,v in re.findall(r'\b([a-z_]+)=(-?\d+)', line)}
    if 'completion wait totals' in line:
        waits.append(r)
        continue
    if r['monotonic_ns'] not in selected:
        continue
    key = (r['asid'], r['width'], r['height'])
    g = geometry.setdefault(key, dict(asid=key[0], width=key[1], height=key[2],
        commands=0, prepare_ns=0, active_ns=0, fragment_ns=0, max_fragment_ns=0))
    for name in ['commands', 'prepare_ns', 'active_ns', 'fragment_ns']:
        g[name] += r[name]
    g['max_fragment_ns'] = max(g['max_fragment_ns'], r['max_fragment_ns'])
if geometry:
    result['render_geometry'] = sorted(geometry.values(), key=lambda g:g['fragment_ns'], reverse=True)
if waits:
    included = [w for w in waits if w['monotonic_ns'] in selected]
    if included and 'consecutive_gap_ns' in included[0]:
        ns = sum(w['consecutive_gap_ns'] for w in included)
        count = sum(w['consecutive_gap_count'] for w in included)
        result['consecutive_command_handoff'] = dict(
            commands=count, seconds=ns/1e9,
            mean_microseconds=ns/1e3/count if count else None,
            scope='Consecutive commands of one ioctl; excludes client interleaving')
    prior = [w for w in waits if w['monotonic_ns'] < min(selected)]
    # Cumulative counters require a preceding sample. Without one, omit the
    # first interval from the deltas rather than including the whole boot.
    if included and (prior or len(included) > 1):
        first = prior[-1] if prior else included[0]
        last = included[-1]
        result['completion_wait_deltas'] = {
            k:last[k]-first[k] for k in ['messages','waits','late','timeouts']}
        result['completion_wait_deltas']['span_seconds'] = (last['monotonic_ns']-first['monotonic_ns'])/1e9
        result['completion_wait_deltas']['geometry_overflow'] = sum(w['geometry_overflow'] for w in included)
# Batch active durations count each packet once. Per-command active spans
# overlap in a batch and cannot be summed as serialized wall time.
counts = {}
packet_ns = 0
for line in lines:
    if 'G16G: batch summary' not in line:
        continue
    r = {k:int(v) for k,v in re.findall(r'\b([a-z_]+)=(-?\d+)', line)}
    if r['monotonic_ns'] not in selected:
        continue
    histogram = re.search(r'packet_counts=(\[[0-9, ]+\])', line)
    if not histogram:
        raise ValueError('Malformed packet histogram')
    for size, count in enumerate(json.loads(histogram[1])):
        if count:
            counts[size] = counts.get(size, 0) + count
    packet_ns += r['packet_active_ns']
if counts:
    result['batches'] = dict(counts_by_commands=counts, active_seconds=packet_ns/1e9,
        scope='Union of published busy intervals' if any('union=1' in line for line in lines) else 'Serialized packets counted once, unlike overlapping per-command active times')
timeline = []
for line in lines:
    if 'G16G: GPU timeline summary' not in line:
        continue
    stamp = re.search(r'monotonic_ns=(\d+)', line)
    if not stamp or int(stamp[1]) not in selected:
        continue
    arrays = {name: json.loads(value) for name, value in
              re.findall(r'(pstate_samples|gap_ticks|gap_counts)=(\[[0-9, ]+\])', line)}
    arrays['overlaps'] = int(re.search(r'overlaps=(\d+)', line)[1])
    timeline.append(arrays)
if timeline:
    states = [sum(t['pstate_samples'][i] for t in timeline) for i in range(16)]
    gaps = [sum(t['gap_ticks'][i] for t in timeline) for i in range(2)]
    counts = [sum(t['gap_counts'][i] for t in timeline) for i in range(2)]
    result['hardware_timeline'] = dict(
        pstate_samples={i:n for i,n in enumerate(states) if n},
        gaps={engine:dict(seconds=ticks/24e6, count=count,
             mean_microseconds=ticks/24/count if count else None)
             for engine,ticks,count in zip(['before_render','before_compute'],gaps,counts)},
        overlaps=sum(t['overlaps'] for t in timeline),
        scope='24 MHz end-to-next-start gaps include firmware/cache work, VM switches and queue starvation; state counts are polling samples, not durations')
boundary = []
for line in lines:
    if 'G16G: GPU boundary summary' not in line:
        continue
    stamp = re.search(r'monotonic_ns=(\d+)', line)
    if not stamp or int(stamp[1]) not in selected:
        continue
    boundary.append({name:json.loads(value) for name,value in
        re.findall(r'(gap_ticks|gap_counts)=(\[[0-9, ]+\])', line)})
if boundary:
    result['packet_boundaries'] = {}
    for i, label in enumerate(['queued_RR','queued_RC','queued_CR','queued_CC',
                               'boundary_RR','boundary_RC','boundary_CR','boundary_CC']):
        ticks=sum(row['gap_ticks'][i] for row in boundary)
        count=sum(row['gap_counts'][i] for row in boundary)
        result['packet_boundaries'][label] = dict(seconds=ticks/24e6, count=count,
            mean_microseconds=ticks/24/count if count else None)
publication = []
for line in lines:
    if 'G16G: GPU publication summary' not in line:
        continue
    stamp = re.search(r'monotonic_ns=(\d+)', line)
    if not stamp or int(stamp[1]) not in selected:
        continue
    match = re.search(r'gap_ticks=(\[.*\]) final_retire=(\[[^]]*\]) invalid=(\d+)', line)
    if not match:
        raise ValueError('Malformed publication timing record')
    publication.append((json.loads(match[1]), json.loads(match[2]), int(match[3])))
if publication:
    result['publication_gaps'] = {
        label: {kind: sum(row[0][i][j] for row in publication)/24e6
                for j,kind in enumerate(['already_queued_seconds', 'before_publication_seconds', 'after_publication_seconds'])}
        for i,label in enumerate(['queued_RR','queued_RC','queued_CR','queued_CC',
                                 'boundary_RR','boundary_RC','boundary_CR','boundary_CC'])}
    count = sum(row[1][0] for row in publication)
    result['final_gpu_end_to_host_retirement'] = dict(count=count,
        mean_microseconds=sum(row[1][1] for row in publication)/24/count if count else None,
        maximum_microseconds=max(row[1][2] for row in publication)/24,
        invalid=sum(row[2] for row in publication),
        note='Physical 24 MHz clock; packet-final GPU end to host retirement. Publication starts before queue producer writes; post-publication gaps include that small host cost.')
print(json.dumps(result,indent=2))
