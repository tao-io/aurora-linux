#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Small client for the root-only M4 DCP diagnostic bridge."""
import os
import select
import struct
import time


class DCP:
    def __init__(self, log, check, callback=None):
        self.fd = os.open('/dev/m4-dcp-rpc', os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
        self.log = log
        self.check = check
        self.callback = callback

    def close(self):
        os.close(self.fd)

    def write(self, kind, message, data):
        record = struct.pack('<IIQ', kind, len(data), message) + data
        if os.write(self.fd, record) != len(record):
            raise RuntimeError('short DCP bridge write')

    def call(self, tag, data=b'', output_size=4, nested=False):
        if len(tag) != 4 or tag[0] != 'A' or not tag[1:].isdigit():
            raise ValueError('expected an A000..A999 method tag')
        wire = int.from_bytes(tag.encode('ascii'), 'big')
        packet = struct.pack('<III', wire, len(data), output_size) + data + bytes(output_size)
        self.check()
        self.log({'event': 'submit', 'tag': tag, 'input': data.hex(),
                  'output_size': output_size, 'nested': nested})
        self.write(1, int(nested), packet)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.check()
            readable, _, _ = select.select([self.fd], [], [], 0.2)
            if not readable:
                continue
            record = os.read(self.fd, 0x10000)
            if len(record) < 16:
                raise RuntimeError('short DCP bridge event')
            kind, size, message = struct.unpack_from('<IIQ', record)
            body = record[16:]
            if size != len(body) or kind not in (1, 2) or size < 12:
                raise RuntimeError(f'unexpected DCP event {record.hex()}')
            call, in_size, out_size = struct.unpack_from('<III', body)
            if 12 + in_size + out_size != size:
                raise RuntimeError('malformed RPC packet')
            name = call.to_bytes(4, 'big').decode('ascii', errors='replace')
            incoming = body[12:12+in_size]
            output = body[12+in_size:]
            self.log({'event': 'reply' if kind == 1 else 'callback', 'tag': name,
                      'message': hex(message), 'input': incoming.hex(),
                      'output_size': out_size, 'output': output.hex()})
            if kind == 1:
                expected_message = 0x142 if nested else 0x42
                if call != wire or message != expected_message or in_size != len(data) or out_size != output_size:
                    raise RuntimeError('unexpected RPC reply header/status')
                return output
            if self.callback is None:
                raise RuntimeError(f'unhandled DCP callback {name}, {in_size}/{out_size} bytes')
            response = self.callback(self, name, incoming, out_size)
            if len(response) != out_size:
                raise RuntimeError(f'{name}: wrong callback output length')
            self.write(2, message, response)
        raise TimeoutError(f'{tag}: DCP reply timed out')
