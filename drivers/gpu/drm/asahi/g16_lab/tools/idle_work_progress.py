"""Bounded experiment evidence: CPU work while no published GPU work is pending.

This supplements, never replaces, completion and thermal monitoring. A caller
must retain its fixed overall deadline and watchdog timeout. CPU work while a
GPU submission is pending is not eligible to renew the watchdog.
"""
import json
from pathlib import Path
import time


def parse_activity(text):
    pairs = [part.split('=') for part in text.split()]
    fields = {'version', 'generation_ns', 'epoch', 'pending', 'healthy'}
    if len(pairs) != len(fields) or any(len(p) != 2 for p in pairs):
        raise RuntimeError('Malformed GPU activity snapshot')
    values = {key: int(value) for key, value in pairs}
    if (set(values) != fields or any(v < 0 or v > 2**64-1 for v in values.values())
            or values['version'] != 1 or not values['generation_ns']
            or values['pending'] not in (0, 1) or values['healthy'] not in (0, 1)
            or values['epoch'] == 0 or values['epoch'] % 2 != values['pending']):
        raise RuntimeError('Invalid GPU activity fields or ABI')
    return values


def process_tree_cpu(root_pid, proc=Path('/proc')):
    """Live descendant CPU ticks, including children that start a new session."""
    processes = {}
    for path in proc.iterdir():
        if not path.name.isdecimal():
            continue
        try:
            with (path/'stat').open() as stream:
                text = stream.read(4097)
        except (FileNotFoundError, ProcessLookupError):
            continue  # A reaped child cannot contribute progress.
        if len(text) > 4096 or ')' not in text:
            raise RuntimeError('Invalid process stat')
        fields = text.rsplit(')', 1)[1].split()  # comm may contain spaces/parentheses.
        if len(fields) < 20:
            raise RuntimeError('Truncated process stat')
        ticks = int(fields[11]) + int(fields[12])  # utime + stime, all threads.
        if ticks < 0:
            raise RuntimeError('Invalid process CPU time')
        processes[int(path.name)] = (int(fields[1]), int(fields[19]), ticks)
    if root_pid not in processes:
        return {}  # A dead launcher cannot qualify its orphaned descendants.
    selected = {root_pid}
    while True:
        children = {pid for pid, (parent, birth, ticks) in processes.items()
                    if parent in selected}
        if children <= selected:
            break
        selected |= children
    return {(pid, processes[pid][1]): processes[pid][2] for pid in selected}



class IdleWorkProgress:
    def __init__(self, evidence, activity_path, generation, process_root,
                 cpu_reader=process_tree_cpu):
        self.evidence = evidence
        self.path = activity_path
        self.generation = generation
        self.root = process_root
        self.cpu_reader = cpu_reader
        self.previous_epoch = 0
        self.previous_cpu = cpu_reader(process_root)
        self.root_identity = next((key for key in self.previous_cpu if key[0] == process_root), None)

    def _read(self):
        with self.path.open() as stream:
            text = stream.read(1025)
        if len(text) > 1024:
            raise RuntimeError('Oversized GPU activity snapshot')
        sample = parse_activity(text)
        # Retain a terminal fault before rejecting it; never advance eligibility.
        with self.evidence.open('a') as log:
            log.write(json.dumps(dict(monotonic=time.monotonic(), **sample))+'\n')
        if not sample['healthy']:
            raise RuntimeError('GPU activity latched unhealthy')
        if sample['generation_ns'] != self.generation:
            raise RuntimeError('GPU activity generation mismatch')
        if sample['epoch'] < self.previous_epoch:
            raise RuntimeError('GPU activity epoch regressed')
        self.previous_epoch = sample['epoch']
        return sample

    def poll(self):
        start = time.monotonic()
        before = self._read()
        current = self.cpu_reader(self.root)
        if current and self.root_identity not in current:
            raise RuntimeError('Experiment launcher identity changed')
        delta = 0
        for identity, ticks in current.items():
            old = self.previous_cpu.get(identity, 0)
            if ticks < old:
                raise RuntimeError('Process CPU time regressed')
            delta += ticks-old
        self.previous_cpu = current
        after = self._read()
        elapsed = time.monotonic()-start
        if not 0 <= elapsed <= 2:
            raise RuntimeError('Stale GPU idle/CPU progress observation')
        eligible = (not after['pending'] and before['epoch'] == after['epoch']
                    and delta > 0)
        with self.evidence.open('a') as log:
            log.write(json.dumps(dict(monotonic=time.monotonic(),
                cpu_ticks_delta=delta, process_root=self.root,
                epoch=after['epoch'], idle_cpu_progress=eligible))+'\n')
        return eligible
