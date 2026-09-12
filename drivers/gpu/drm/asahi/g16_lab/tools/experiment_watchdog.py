#!/usr/bin/env python3
"""Arm the J713 hardware watchdog before a bounded experiment, without feeding it."""
import argparse
import array
import fcntl
import json
import os
import subprocess
import socket
import time

# Linux arm64 UAPI, include/uapi/linux/watchdog.h.
GETSUPPORT = 0x80285700
SETOPTIONS = 0x80045704
SETTIMEOUT = 0xc0045706
GETTIMEOUT = 0x80045707
GETTIMELEFT = 0x8004570a
KEEPALIVE = 0x80045705


class ExperimentWatchdog:
    def __init__(self):
        self.fd = None
        self.started = None
        self.broker = None

    def integer(self, request, value=0):
        data = array.array('i', [value])
        fcntl.ioctl(self.fd, request, data, True)
        return data[0]

    def arm(self):
        broker_path = '/run/m4-ssh-watchdog.sock'
        if os.path.exists(broker_path):
            self.broker = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            self.broker.settimeout(3)
            self.broker.connect(broker_path)
            self.broker.send(b'acquire')
            message, ancillary, flags, _ = self.broker.recvmsg(1024, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
            descriptors = []
            for level, kind, data in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    values = array.array('i')
                    values.frombytes(data)
                    descriptors.extend(values)
            if not json.loads(message).get('leased') or len(descriptors) != 1:
                for fd in descriptors:
                    os.close(fd)
                raise RuntimeError('Watchdog broker refused experiment lease: ' + message.decode())
            self.fd = descriptors[0]
        # Opening starts the watchdog. CLOEXEC keeps ownership in this process.
        else:
            self.fd = os.open('/dev/watchdog0', os.O_WRONLY | os.O_CLOEXEC)
        try:
            info = bytearray(40)
            fcntl.ioctl(self.fd, GETSUPPORT, info, True)
            identity = bytes(info[8:]).split(b'\0')[0].decode('ascii')
            if identity != 'Apple SoC Watchdog':
                raise RuntimeError(f'unexpected watchdog: {identity}')
            accepted = self.integer(SETTIMEOUT, 60)
            self.started = time.monotonic()
            timeout = self.integer(GETTIMEOUT)
            left = self.integer(GETTIMELEFT)
            if accepted != 60 or timeout != 60 or not 50 <= left <= 60:
                raise RuntimeError(f'watchdog timeout verification failed: {accepted}, {timeout}, {left}')
        except BaseException:
            # Setup failed; no experiment has started, so stop explicitly.
            self.disarm()
            raise
        evidence = {'device': '/dev/watchdog0', 'identity': identity,
                    'timeout_seconds': timeout, 'initial_timeleft_seconds': left,
                    'periodic_keepalive': False, 'armed': True}
        evidence['ssh_broker_lease'] = self.broker is not None
        print('Watchdog armed: ' + json.dumps(evidence), flush=True)
        return evidence

    def disarm(self):
        if self.broker is not None:
            # Return idle ownership only after the caller saved evidence.
            # The broker never feeds on SSH pings while we hold this lease.
            self.broker.send(b'release')
            if not json.loads(self.broker.recv(1024)).get('released'):
                raise RuntimeError('Watchdog broker refused release')
            self.broker.close()
            self.broker = None
            os.close(self.fd)
            self.fd = None
            return
        # Never call from a finally block around an experiment: exceptions or
        # a stuck child must leave recovery armed. DISABLECARD reports errors,
        # unlike relying only on magic close. No heartbeat thread is started.
        self.integer(SETOPTIONS, 1)
        os.close(self.fd)
        self.fd = None

    def keepalive_after_gpu_check(self, check):
        # No timer thread. A hung GPU or compositor cannot reach this call.
        if not (check.get('passed') is True and check.get('shader_submitted') is True
                and check.get('vulkan') is True and check.get('frames') == 8
                and check.get('device_id') == 0x8132
                and check.get('pixels_verified') == 524288
                and check.get('compositor_responsive') is True):
            raise RuntimeError('refusing watchdog keepalive without verified GPU/compositor progress')
        self.integer(KEEPALIVE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--expire-test', action='store_true',
                        help='deliberately wait for the 60-second hardware reset')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command[:1] == ['--']:
        command = command[1:]
    if args.expire_test == bool(command):
        parser.error('choose either --expire-test or a command after --')
    os.sync()
    watchdog = ExperimentWatchdog()
    watchdog.arm()
    if args.expire_test:
        print('Expiry test: waiting for hardware reset; no keepalives.', flush=True)
        time.sleep(90)
        raise RuntimeError('hardware did not reset within 90 seconds')
    run = subprocess.run(command)
    watchdog.disarm()
    print(f'Watchdog disarmed after command returned {run.returncode}.', flush=True)
    return run.returncode


if __name__ == '__main__':
    raise SystemExit(main())
