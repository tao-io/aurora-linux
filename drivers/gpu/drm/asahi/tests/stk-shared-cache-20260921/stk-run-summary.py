#!/usr/bin/env python3
"""Offline race FPS, memory, temperature and render-preparation comparisons.

Consumes gpu-experiment evidence from stk-live-benchmark.py; never runs hardware.
Reuse counts include startup, and preparation averages cover the selected
application's 4K passes, not an identical frame sequence or GPU utilization.
"""
import argparse
import collections
import json
from pathlib import Path
import re


def summarize(run):
    remote=run/'remote'
    report=json.loads((run/'report.json').read_text())
    result=dict(run=run.name,guard_verdict=report['verdict'])
    if (remote/'benchmark-summary.json').exists():
        result.update(json.loads((remote/'benchmark-summary.json').read_text()))
    samples=[json.loads(l) for l in (remote/'memory-samples.jsonl').read_text().splitlines()]
    result['minimum_available_mib']=min(int(re.search(r'^MemAvailable:\s+(\d+)',s['meminfo'],re.M)[1])/1024 for s in samples)
    result['peak_temperature_c']=report['observations']['peak_temperature_c']
    log=(remote/'kernel-live.log').read_text()
    counts=collections.Counter(map(int,re.findall(r'owned render 3840x2160 ASID=(\d+)',log)))
    if counts:
        asid=counts.most_common(1)[0][0]
        result['application_asid']=asid
        rows=[]
        for line in log.splitlines():
            if 'render geometry summary' not in line: continue
            r={k:int(v) for k,v in re.findall(r'\b([a-z_]+)=(-?\d+)',line)}
            if (r['asid'],r['width'],r['height'])==(asid,3840,2160): rows.append(r)
        commands=sum(r['commands'] for r in rows)
        result['full_size_render_commands']=commands
        if commands:
            result['mean_full_size_prepare_ms']=sum(r['prepare_ns'] for r in rows)/commands/1e6
        result['render_owner_reuse_including_startup']=dict(collections.Counter(
            re.findall(r'owned render .*? ASID='+str(asid)+r' .*?reused=(\w+)',log)))
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('runs',type=Path,nargs='+')
    p.add_argument('--output',type=Path)
    a=p.parse_args()
    result=[summarize(run) for run in a.runs]
    text=json.dumps(result,indent=2)+'\n'
    if a.output: a.output.write_text(text)
    print(text,end='')
