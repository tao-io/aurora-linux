#!/usr/bin/env python3
"""Pack verified encoded-stage M4 resources; never include reference output."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct

PAGE = 0x4000


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('reference', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    m = json.loads((a.reference / 'reference.json').read_text())
    resources = m['resources']
    if len(resources) != 34:
        raise ValueError('requires the 34-mapping M4 draw fixture')
    blob = bytearray(PAGE)
    struct.pack_into('<8sII', blob, 0, b'G16DRAW1', len(resources), 0)
    ranges = []
    for i, r in enumerate(resources):
        if not r['source_file'].startswith('encoded/'):
            raise ValueError('only pre-execution resources may be uploaded')
        data = gzip.decompress((a.reference / r['file']).read_bytes())
        va = int(r['gpu_va'], 16)
        size = len(data)
        if (size != r['size'] or hashlib.sha256(data).hexdigest() != r['sha256']):
            raise ValueError('resource checksum/size mismatch: ' + r['file'])
        if not size or size % PAGE or va % PAGE or not 0 < va < va + size <= 1 << 42:
            raise ValueError('invalid mapping')
        if any(va < end and start < va + size for start, end in ranges):
            raise ValueError('overlapping mappings')
        ranges.append((va, va + size))
        struct.pack_into('<QII', blob, 16 + i * 16, va, size, len(blob))
        blob.extend(data)
    # Exclusive creation prevents accidentally replacing a fixture in use.
    with a.output.open('xb') as f:
        f.write(blob)
    print(f'{len(resources)} mappings, {len(blob)} bytes, sha256={hashlib.sha256(blob).hexdigest()}')


if __name__ == '__main__':
    main()
