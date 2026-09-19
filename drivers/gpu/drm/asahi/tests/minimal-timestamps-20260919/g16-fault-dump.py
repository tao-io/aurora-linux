#!/usr/bin/env python3
"""Collect sealed M4FWD001 device coredumps, or decode them offline."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct


def decode(raw):
    if len(raw) < 40 or raw[:8] != b'M4FWD001':
        raise ValueError('not an M4FWD001 dump')
    count, flags, job, created, length = struct.unpack_from('<IIQQQ', raw, 8)
    if flags != 1 or length != len(raw):
        raise ValueError('unsealed or truncated dump')
    records, cursor = [], 40
    for _ in range(count):
        if cursor+64 > len(raw):
            raise ValueError('truncated record header')
        name_bytes = raw[cursor:cursor+32]
        name = name_bytes.split(b'\0', 1)[0].decode('ascii')
        if not name or len(name) >= 32 or any(name_bytes[len(name):]):
            raise ValueError('invalid record name')
        va, size, begin, end = struct.unpack_from('<4Q', raw, cursor+32)
        cursor += 64
        if not size or cursor+size > len(raw) or end < begin:
            raise ValueError('invalid record bounds or interval')
        records.append({'name': name, 'va': hex(va), 'size': size, 'offset': cursor,
                        'begin_monotonic_ns': begin, 'end_monotonic_ns': end,
                        'sha256': hashlib.sha256(raw[cursor:cursor+size]).hexdigest()})
        cursor += size
    if cursor != len(raw) or len({r['name'] for r in records}) != count:
        raise ValueError('trailing bytes or duplicate record names')
    return {'job': job, 'created_monotonic_ns': created, 'length': length, 'records': records,
            'sha256': hashlib.sha256(raw).hexdigest(),
            'note': 'Sealed fault-only sequential observations. No atomicity or completion claim.'}


def collect(args):
    spec = importlib.util.spec_from_file_location('snapshot_guard', Path(__file__).with_name('firmware-snapshot.py'))
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    helper.check_guard(args.guard_output, boot)
    start_time = (args.guard_output/'start.json').stat().st_mtime_ns
    captured = []
    for source in sorted(Path('/sys/class/devcoredump').glob('devcd*/data')):
        # Do not attach a previous experiment's dump to this result.
        if source.stat().st_ctime_ns < start_time:
            continue
        with source.open('rb', buffering=0) as stream:
            header = stream.read(40)
            if header[:8] != b'M4FWD001':
                continue
            if len(header) != 40:
                raise ValueError('partial M4 dump header')
            length = struct.unpack_from('<Q', header, 32)[0]
            if not 40 <= length <= 16*1024*1024:
                raise ValueError('dump exceeds collection bound')
            raw = bytearray(header)
            while len(raw) < length:
                chunk = stream.read(min(length-len(raw), 65536))
                if not chunk:
                    raise ValueError('early EOF in M4 dump')
                raw.extend(chunk)
        report = decode(raw)
        name = f'g16-fault-{source.parent.name}-job{report["job"]}.bin'
        with (args.guard_output/name).open('xb') as target:
            target.write(raw)
            target.flush()
            os.fsync(target.fileno())
        report.update(source=str(source), boot_id=boot, file=name)
        with (args.guard_output/(name+'.json')).open('x') as target:
            target.write(json.dumps(report, indent=2)+'\n')
            target.flush()
            os.fsync(target.fileno())
        captured.append(name)
    helper.check_guard(args.guard_output, boot)
    if not captured and not args.allow_missing:
        raise RuntimeError('no new sealed M4 coredump')
    print('M4 fault dumps collected: '+json.dumps(captured), flush=True)


def scheduler(raw, report):
    """Decode observed 25G83 scheduling fields, without inferring completion."""
    records = {r['name']: r for r in report['records']}
    def blob(name, size):
        r = records[name]
        if r['size'] != size:
            raise ValueError('unexpected 25G83 record size: '+name)
        return raw[r['offset']:r['offset']+size]
    def u32(b, off): return struct.unpack_from('<I', b, off)[0]
    def u64(b, off): return struct.unpack_from('<Q', b, off)[0]
    data = blob('firmware-data', 0x13c000)
    result = {'job': report['job'], 'dump_sha256': report['sha256'],
              'gate_192900': hex(u32(data, 0x192900-0x60000)),
              'free_engine_mask_128d60': hex(u64(data, 0x128d60-0x60000)),
              'queues': {}, 'note': report['note']}
    result['firmware_timing'] = {
        'reference_clock_khz_193b6c': u32(data, 0x193b6c-0x60000),
        'maintenance_period_193b70': u32(data, 0x193b70-0x60000),
        'power_sample_period_193b74': u32(data, 0x193b74-0x60000),
        'maintenance_ticks_128da0': u32(data, 0x128da0-0x60000),
        'derived_timer_interval_1928e0': u64(data, 0x1928e0-0x60000)}
    runtime = blob('runtime', 0x4b8)
    result['priority_weight_totals_304'] = [u32(runtime, 0x304+4*i) for i in range(4)]
    result['priority_shifts_314'] = [u32(runtime, 0x314+4*i) for i in range(4)]
    result['accounting_dirty_324'] = u32(runtime, 0x324)
    for stage in ('ta', 'fragment', 'compute'):
        q = blob('queue-'+stage, 0x24c0)
        entry = {'info_cursors_20_24_28': [u32(q, o) for o in (0x20, 0x24, 0x28)],
                 'scheduler_slot_2c': hex(u32(q, 0x2c)), 'active_nodes': []}
        for index in range(128):
            node = q[0xb0+index*0x48:0xb0+(index+1)*0x48]
            if not any(node) or u32(node, 0) == 3:
                continue
            entry['active_nodes'].append({'index': index, 'state': u32(node, 0),
                'flags_04': hex(u32(node, 4)), 'command_and_flags_10': hex(u64(node, 0x10)),
                'sequence_28': hex(u64(node, 0x28)), 'restart_30': hex(u64(node, 0x30)),
                'engine_44': node[0x44], 'flags_45': hex(node[0x45]),
                'uma_class_46': node[0x46] & 3})
        context_name = 'queue-context' if 'queue-context' in records else 'context-'+stage
        if context_name in records:
            c = blob(context_name, 0x38)
            if u64(q, 0xa4) != int(records[context_name]['va'], 16):
                raise ValueError('queue/context pointer mismatch')
            entry['context'] = {'record': context_name, 'group_00': c[0], 'slot_01': c[1], 'priority_03': c[3],
                'queue_weight_04': c[4], 'weight_share_06': u32(c, 6),
                'timeslice_0a': u32(c, 0xa), 'process_weight_26': c[0x26]}
            ring = blob('ring-state-'+stage, 0x60)
            if u64(q, 0) != int(records['ring-state-'+stage]['va'], 16):
                raise ValueError('queue/ring pointer mismatch')
            entry['ring_cursors_00_30_40'] = [u32(ring, o) for o in (0, 0x30, 0x40)]
        result['queues'][stage] = entry
    for name, key in [('client-notifications', 'client_notifications'),
                      ('render-client-notifications', 'render_client_notifications')]:
        if name not in records:
            continue
        client = blob(name, 0x1000)
        result[key] = {
            stage: dict(submitted=u64(client, 0x600 + i*8),
                        firmware_stamp=hex(u32(client, 0x620 + i*16)),
                        published_stamp=hex(u32(client, 0x628 + i*16)),
                        flush_requested=u32(client, i*0x200 + 0x20),
                        asid=u32(client, i*0x200 + 0x24))
            for i, stage in enumerate(('ta', 'fragment', 'compute'))}
    if 'compute-command' in records:
        command_size = records['compute-command']['size']
        if command_size not in (0x1000, 0x2000):
            raise ValueError('unexpected compute command ABI size')
        command = blob('compute-command', command_size)
        registers = u32(command, 0x728) & 0xffff
        if registers > (0x720-0x20)//12:
            raise ValueError('compute register list exceeds command record')
        result['compute_command'] = {
            'va': records['compute-command']['va'],
            'context_0c': u32(command, 0xc), 'generation_88b': command[0x88b],
            'register_list_gpu_720': hex(u64(command, 0x720)),
            'registers': {hex(u32(command,0x20+i*12)):hex(u64(command,0x24+i*12))
                          for i in range(registers)},
            'stamp_810': hex(u32(command,0x810)),
            'completion_ee0': [hex(u32(command,o)) for o in (0xee0,0xee4)],
            'timestamps_8c0': [hex(u64(command,o)) for o in (0x8c0,0x8c8)],
            'uma_descriptor_e00': {hex(o):hex(u32(command,0xe00+o)) for o in range(0,0xa0,4)},
            'uma_slot1_words': [hex(u64(blob('uma-table',0x4000),32+i*8)) for i in range(4)]}
        if 'compute-command-mappings' in records:
            m = blob('compute-command-mappings',24)
            result['compute_command']['mappings'] = {
                'gpu_va':hex(u64(m,0)), 'bootstrap_pa':hex(u64(m,8)),
                'client_pa':hex(u64(m,16)), 'same_physical_page':u64(m,8)==u64(m,16)}
        if 'compute-retired-command' in records:
            previous_size = records['compute-retired-command']['size']
            if previous_size not in (0x1000, 0x2000):
                raise ValueError('unexpected retired compute command ABI size')
            previous = blob('compute-retired-command',previous_size)
            result['compute_command']['previous'] = {
                'va':records['compute-retired-command']['va'],
                'uma_descriptor_e00':{hex(o):hex(u32(previous,0xe00+o)) for o in range(0,0xa0,4)}}
    if 'render-parameter-manager' in records:
        manager = blob('render-parameter-manager', 0xc0)
        ring = blob('render-parameter-ring', 8)
        scene = blob('render-scene-metadata', 0x4000)
        ta = blob('render-ta-command', 0x940)
        fragment = blob('render-fragment-command', 0xca0)
        if (u64(ta, 0x20) != int(records['render-parameter-manager']['va'], 16) or
            u64(ta, 0x28) != int(records['render-scene-metadata']['va'], 16) or
            u64(scene, 0x40) != int(records['render-scene-metadata']['va'], 16)+0x200):
            raise ValueError('render allocator ownership pointer mismatch')
        # Compare address bits only: the hardware owns descriptor cursors and
        # status flags. The normal VA is compacted by the native PB encoder.
        pages = u64(manager, 0x28)
        bias = 0 if pages & (1 << 42) else 0x7000000000
        expected_address_words = [(pages >> 4) & 0xfffffff8,
            (((pages+bias) >> 8) & 0x70000000) | ((pages >> 11) & 0x80000000)]
        slot = u32(manager, 0xc)
        def selected_buffer(command, base, register):
            count = struct.unpack_from('<H', command, base+0x708)[0]
            if count > 0x600//12:
                raise ValueError('render register list exceeds aperture')
            values = [u64(command, base+i*12+4) for i in range(count)
                      if u32(command, base+i*12) & ~1 == register]
            if len(values) != 1:
                raise ValueError('missing or duplicated parameter-buffer selector')
            return values[0]
        selectors = {'manager_0c':slot, 'command_18':u32(ta, 0x18),
            'ta_register_1c830':selected_buffer(ta, 0x40, 0x1c830),
            'fragment_register_1c838':selected_buffer(fragment, 0x80, 0x1c838)}
        descriptor = None
        if 'parameter-table' in records and slot < 64:
            table = blob('parameter-table', 0x4000)
            words = [u32(table, slot*16+i*4) for i in range(4)]
            descriptor = dict(slot=slot, words=[hex(w) for w in words],
                expected_address_words=[hex(w) for w in expected_address_words],
                matches_manager_address=(words[0] & 0xfffffff8 == expected_address_words[0]
                    and words[1] & 0xf0000000 == expected_address_words[1]))
        result['parameter_buffer'] = {
            'owner_selectors':selectors,
            'owner_selectors_agree':len(set(selectors.values())) == 1,
            'page_list_gpu_va_28': hex(pages),
            'hardware_descriptor': descriptor,
            'init_cursor_0c': u32(scene, 0x30c),
            'init_staged_10': u32(scene, 0x310),
            'populated_pages_34': u32(manager, 0x34),
            'block_capacity_38': u32(manager, 0x38),
            'block_write_3c': u32(manager, 0x3c), 'block_read_40': u32(manager, 0x40),
            'last_page_54': u32(manager, 0x54),
            'limits_7c_80': [u32(manager, o) for o in (0x7c, 0x80)],
            'ring_00_04': [u32(ring, 0), u32(ring, 4)],
            'scene_counts_10_48': [u32(scene, 0x10), u32(scene, 0x48)],
            'statistics_200': [u32(scene, 0x200+i*4) for i in range(9)],
            'ta_fragment_slot_77c': u32(ta, 0x77c),
            'ta_fragment_stamp_780': hex(u32(ta, 0x780)),
            'fragment_event_slot_bd4': u32(fragment, 0xbd4),
            'fragment_event_stamp_bd0': hex(u32(fragment, 0xbd0)),
            'note': 'Counts are firmware snapshots, not proof of exact utilization or retirement.'}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest='mode', required=True)
    sub = subs.add_parser('collect')
    sub.add_argument('--guard-output', type=Path, required=True)
    sub.add_argument('--allow-missing', action='store_true')
    sub = subs.add_parser('decode')
    sub.add_argument('dump', type=Path)
    sub.add_argument('--extract', type=Path)
    sub.add_argument('--scheduler', action='store_true', help='summarize 25G83 queue/context fields')
    args = parser.parse_args()
    if args.mode == 'collect':
        collect(args)
    else:
        raw = args.dump.read_bytes()
        report = decode(raw)
        if args.extract:
            args.extract.mkdir(exist_ok=False)
            for r in report['records']:
                if any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in r['name']):
                    raise ValueError('unsafe extraction name')
                (args.extract/(r['name']+'.bin')).write_bytes(raw[r['offset']:r['offset']+r['size']])
            (args.extract/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(scheduler(raw, report) if args.scheduler else report, indent=2))


if __name__ == '__main__':
    main()
