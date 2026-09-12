#!/usr/bin/env python3
"""J713 watchdog broker and host-side SSH heartbeat. See README-watchdog.md."""
import argparse
import array
import fcntl
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import time

SOCKET = '/run/m4-ssh-watchdog.sock'
SETTIMEOUT, GETTIMEOUT, GETTIMELEFT, KEEPALIVE = 0xc0045706, 0x80045707, 0x8004570a, 0x80045705

def integer(fd, request, value=0):
    data = array.array('i', [value])
    fcntl.ioctl(fd, request, data, True)
    return data[0]

def log(**fields):
    print(json.dumps(dict(time=time.time(), **fields)), flush=True)

def broker():
    # Never replace a failed broker during the same boot: its watchdog may
    # already be counting down after a test failure.
    marker = Path('/run/m4-ssh-watchdog.started')
    with marker.open('x') as stream:
        stream.write(Path('/proc/sys/kernel/random/boot_id').read_text())
    fd = os.open('/dev/watchdog0', os.O_WRONLY | os.O_CLOEXEC)
    info = bytearray(40)
    fcntl.ioctl(fd, 0x80285700, info, True)
    identity = bytes(info[8:]).split(b'\0')[0].decode('ascii')
    if identity != 'Apple SoC Watchdog':
        raise RuntimeError('Unexpected watchdog: ' + identity)
    if integer(fd, SETTIMEOUT, 60) != 60 or integer(fd, GETTIMEOUT) != 60:
        raise RuntimeError('Watchdog rejected 60 seconds')
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(SOCKET)
    os.chmod(SOCKET, 0o600)
    server.listen(8)
    lease = None
    failed = False
    log(event='armed', pid=os.getpid(), timeout=60, timeleft=integer(fd, GETTIMELEFT))
    while True:
        ready, _, _ = select.select([server] + ([lease] if lease else []), [], [])
        if lease and lease in ready:
            data = lease.recv(1024)
            if data == b'release':
                integer(fd, KEEPALIVE)
                lease.send(b'{"released":true}')
                log(event='lease-released')
            else:
                # A crash/exception must not be hidden by an idle SSH ping.
                failed = True
                log(event='lease-failed', recovery='allow watchdog expiry')
            lease.close()
            lease = None
        if server not in ready:
            continue
        conn, _ = server.accept()
        conn.settimeout(2)
        try:
            pid, uid, gid = struct.unpack('3i', conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if uid != 0:
                raise RuntimeError('Root peer required')
            command = conn.recv(1024)
            if command == b'acquire' and not lease and not failed:
                integer(fd, KEEPALIVE)
                conn.sendmsg([b'{"leased":true}'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
                conn.settimeout(None)
                lease, conn = conn, None
                log(event='lease-acquired', guard_pid=pid)
            elif command in (b'ping', b'status'):
                feed = command == b'ping' and not lease and not failed
                if feed:
                    integer(fd, KEEPALIVE)
                status = dict(armed=True, timeout_seconds=integer(fd, GETTIMEOUT),
                              timeleft_seconds=integer(fd, GETTIMELEFT), fed=feed,
                              mode='failed' if failed else 'experiment' if lease else 'idle')
                conn.send(json.dumps(status).encode())
                log(event=command.decode(), **status)
            else:
                conn.send(b'{"error":"Watchdog busy or recovery latched"}')
        except (OSError, RuntimeError) as error:
            log(event='client-error', error=str(error))
        finally:
            if conn:
                conn.close()

def request(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as conn:
        conn.settimeout(3)
        conn.connect(SOCKET)
        conn.send(command.encode())
        print(conn.recv(4096).decode())

def heartbeat(host):
    # A host process owns the timer. No local target timer feeds the device.
    # /run clears at reboot, allowing a fresh broker to be armed on recovery.
    command = 'systemctl start m4-ssh-watchdog.service && python3 /root/ssh-watchdog.py ping'
    while True:
        start = time.monotonic()
        try:
            result = subprocess.run(['ssh', '-oBatchMode=yes', '-oConnectTimeout=5',
                '-oServerAliveInterval=3', '-oServerAliveCountMax=1', host, command],
                text=True, capture_output=True, timeout=8)
            log(event='ssh-heartbeat', returncode=result.returncode,
                output=result.stdout.strip(), error=result.stderr.strip())
        except subprocess.TimeoutExpired:
            log(event='ssh-timeout')
        time.sleep(max(0, 10 - (time.monotonic() - start)))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['broker', 'heartbeat', 'ping', 'status'])
    parser.add_argument('--host', default='root@omarchy-m4')
    args = parser.parse_args()
    if args.mode == 'broker':
        broker()
    elif args.mode == 'heartbeat':
        heartbeat(args.host)
    else:
        request(args.mode)
