#!/usr/bin/env python3
"""Preserve a checksummed J713 draw reference without touching the GPU."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct

PAGE = 0x4000

def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.capture.resolve()
    checksums = {}
    for line in (source / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split(maxsplit=1)
        checksums[name.lstrip('*')] = digest

    def read(relative):
        relative = str(relative)
        data = (source / relative).read_bytes()
        if checksums.get(relative) != sha(data):
            raise ValueError('checksum mismatch or absent checksum: ' + relative)
        return data

    manifest = json.loads(read('encoded/manifest.json'))
    if manifest['layout'] != 'macOS-25G83-IOGPU' or 'device=Apple M4 ' not in read('metalprobe.log').decode():
        raise ValueError('requires the recorded M4/25G83 layout')
    if 'render-draw' not in str(manifest['probe_command']):
        raise ValueError('requires the commandprobe render-draw case')
    output = args.output
    if output.exists():
        raise ValueError('output directory already exists')
    files = {}
    objects = manifest['namespaces']['resources']['objects']
    unique = {}
    resources = []
    for obj in objects:
        payload = obj.get('payload_file')
        if not payload:
            continue
        data = read(Path('encoded') / payload)
        if len(data) != obj['mapped_length'] or len(data) % PAGE:
            raise ValueError('unexpected payload size')
        for mapping in obj['mappings']:
            va = int(mapping['gpu_va'], 16)
            if va % PAGE or va + len(data) > 1 << 42:
                raise ValueError('invalid GPU mapping')
            if va in unique:
                if unique[va] != data:
                    raise ValueError('same VA has different resource contents')
                continue
            for oldva, olddata in unique.items():
                if va < oldva + len(olddata) and oldva < va + len(data):
                    raise ValueError('partially overlapping capture mappings')
            unique[va] = data
            name = f'resource-{obj["id"]:05d}.bin.gz'
            files[name] = gzip.compress(data, mtime=0)
            resources.append({'resource_id':obj['id'], 'gpu_va':hex(va),
                              'size':len(data), 'sha256':sha(data), 'file':name,
                              'source_file':'encoded/' + payload})

    # The commandprobe creates a compute pipeline first, then its render pair.
    # These record positions are a capture fact, not an ISA stage selector.
    arena = unique[0x10000000000]
    offset = 0
    records = []
    while offset + 4 <= len(arena):
        size = struct.unpack_from('<I', arena, offset)[0]
        if not size:
            break
        if size % 0x40 or size < 0x40 or offset + size > len(arena):
            raise ValueError('malformed shader arena')
        if offset:
            if size < 0x80:
                raise ValueError('short shader record')
            storage = arena[offset + 0x80:offset + size]
            last = max((i for i, byte in enumerate(storage) if byte), default=-1)
            code = storage[:(last + 2) & ~1]
            records.append((offset + 0x80, code))
        offset += size
    shaders = []
    for stage, index in [('fragment', 1), ('vertex', 2)]:
        start, code = records[index]
        if not code.endswith(b'\x0e\x00'):
            raise ValueError('missing shader stop')
        if stage == 'vertex' and not code.startswith(bytes.fromhex('0cdd1006')):
            raise ValueError('vertex shader does not start with captured sr93 read')
        files[stage + '.bin'] = code
        shaders.append({'stage':stage, 'record_index':index, 'arena_offset':hex(start),
                        'size':len(code), 'sha256':sha(code)})

    texture = next(obj for obj in objects if obj['id'] == 19)
    before = read(Path('encoded') / texture['payload_file'])
    after = read(Path('complete') / texture['payload_file'])
    changes = [i for i, (a,b) in enumerate(zip(before,after)) if a != b]
    if changes != list(range(0x100, 0x140)) or after[0x100:0x140] != struct.pack('<I',0xffbf8040)*16:
        raise ValueError('unexpected native render-target output')
    files['expected-tile.rgba'] = after[0x100:0x140]
    metadata = {
        'source':str(source), 'capture_manifest_sha256':sha(read('encoded/manifest.json')),
        'status':'native M4 reference only; no Linux vertex/fragment execution yet',
        'width':4, 'height':4, 'resources':resources, 'shaders':shaders,
        'render_target':{'gpu_va':hex(0x10000058000), 'tile_offset':hex(0x100),
                         'pixel_rgba8_le':hex(0xffbf8040), 'pixels':16},
    }
    files['reference.json'] = (json.dumps(metadata,indent=2)+'\n').encode()
    output.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        (output / name).write_bytes(data)
    print(f'Verified {len(resources)} unique GPU mappings and native 4x4 output.')
    print(json.dumps(shaders,indent=2))

if __name__ == '__main__':
    main()
