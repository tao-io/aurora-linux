#!/usr/bin/env python3
import argparse, json, os, re, signal, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, '/root/g16-submit-test')
from experiment_watchdog import ExperimentWatchdog, KEEPALIVE
from experiment_thermals import ExperimentThermals
p=argparse.ArgumentParser()
p.add_argument('--output', required=True, type=Path)
p.add_argument('--seconds', type=int, default=40)
p.add_argument('--cpu-max-khz', type=int, default=1860000, choices=[1860000, 4104000],
               help='CPU ceiling during the run; the thermal guard only lowers existing limits')
p.add_argument('--thermal-stop-mc', type=int, choices=[95000, 100000, 120000], default=120000,
               help='May tighten, never raise, the installed thermal monitor limit')
p.add_argument('--trace-kernel-writes',action='store_true',help='Record failed kernel-log write syscalls without relaxing the guard')
p.add_argument('command', nargs=argparse.REMAINDER)
a=p.parse_args()
assert 1 <= a.seconds <= 1800
command=a.command[1:] if a.command[:1]==['--'] else a.command
assert command
a.output.mkdir(parents=True, exist_ok=False)
w=ExperimentWatchdog()
r={'command':command, 'seconds':a.seconds, 'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 'watchdog':w.arm()}
(a.output/'start.json').write_text(json.dumps(r,indent=2)+'\n')
os.sync()
initial_taint=int(Path("/proc/sys/kernel/tainted").read_text())
initial_dmesg=subprocess.check_output(["dmesg","--color=never"],text=True)
proc=None
def stop():
    if proc is not None and proc.poll() is None:
        os.killpg(proc.pid, signal.SIGTERM)
os.environ['M4_TEST_CPU_MAX_KHZ']=str(a.cpu_max_khz)
t=ExperimentThermals(a.output,on_trip=stop)
if a.thermal_stop_mc > t.stop_limit_mc:
    w.disarm()
    raise RuntimeError('Cannot raise the installed thermal monitor limit')
t.stop_limit_mc = a.thermal_stop_mc
try:t.start()
except BaseException:
    w.disarm()
    raise
kernel_proc=None
kernel_read=None
kernel_write=None
kernel_pending=''
last_feed=time.monotonic()
last_job=-1
progress = None
progress_path = Path('/sys/kernel/debug/asahi-m4/progress')
if progress_path.exists():
    from completion_progress import CompletionProgress
    progress = CompletionProgress(a.output/'host-progress.jsonl', progress_path)
    r['progress_source'] = 'debugfs-host-verified-v1'
else:
    # Explicit compatibility for older deployed kernels. A present endpoint
    # that is malformed/unhealthy never falls back to printk-based renewal.
    r['progress_source'] = 'legacy-printk-retirement'
(a.output/'start.json').write_text(json.dumps(r,indent=2)+'\n')
progress_error = None
idle_work = None
renewed_count = 0
# Retain the entire experiment stream even for short runs. This remains the
# authoritative fault evidence if the optional journal query stalls.
kernel_write=(a.output/'kernel-live.log').open('w')
kernel_command=['dmesg','--follow-new','--color=never']
if a.trace_kernel_writes:
    kernel_command=['strace','-qq','--status=failed','-e','trace=write,writev,close','-o',str(a.output/'kernel-write-errors.log'),'--',*kernel_command]
kernel_proc=subprocess.Popen(kernel_command,stdout=kernel_write,stderr=subprocess.STDOUT,start_new_session=True)
kernel_read=(a.output/'kernel-live.log').open()
with (a.output/'command.log').open('w') as log:
    os.fsync(log.fileno())
    os.sync()
    proc=subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    if progress is not None and progress_path.with_name('activity').exists():
        from idle_work_progress import IdleWorkProgress
        idle_work = IdleWorkProgress(a.output/'idle-cpu-progress.jsonl',
            progress_path.with_name('activity'), progress.previous['generation_ns'], proc.pid)
        r['idle_cpu_source'] = 'host-activity-v1-and-process-tree-cpu'
    deadline=time.monotonic()+a.seconds
    streamed=0
    def relay():
        global streamed
        with (a.output/'command.log').open('rb') as source:
            source.seek(streamed)
            chunk=source.read()
        if chunk:
            streamed += len(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
    while proc.poll() is None:
        relay()
        t.check()
        if progress is not None:
            try:
                count, completed_at = progress.poll(time.monotonic())
                idle_advanced = idle_work.poll() if idle_work is not None else False
                now = time.monotonic()
                fresh_retirement = (count > renewed_count and completed_at is not None
                                    and 0 <= now-completed_at < 3)
                if (a.seconds > 40 and (fresh_retirement or idle_advanced) and
                        now-last_feed >= 5):
                    t.check()
                    with (a.output/'host-progress.jsonl').open('a') as evidence:
                        evidence.flush(); os.fsync(evidence.fileno())
                    w.integer(KEEPALIVE)
                    last_feed = now
                    renewed_count = count
                    print(json.dumps({'event':'idle-cpu-renewal' if idle_advanced else 'gpu-retirement-renewal','completed':count}),flush=True)
            except (OSError, RuntimeError, ValueError) as error:
                progress_error = str(error)
                r['progress_error'] = progress_error
                stop()
        if kernel_read is not None:
            if kernel_proc.poll() is not None:
                stop()
                raise RuntimeError(f'kernel progress monitor stopped (exit {kernel_proc.returncode}); watchdog remains armed')
            kernel_pending += kernel_read.read()
            lines=kernel_pending.split('\n')
            kernel_pending=lines.pop()
            for line in lines:
                if any(f in line for f in ('GPU retirement unproven','first runtime error','firmware crashed','Internal error:','BUG:')):
                    stop()
                match=re.search(r'^\[\s*([0-9.]+)\].*userspace GPU job (\d+) retired',line)
                now=time.monotonic()
                exact=re.search(r'\bmonotonic_ns=(\d+)',line)
                retired_at=int(exact[1])/1e9 if exact else (float(match[1]) if match else 0)
                if match and int(match[2])>last_job and 0<=now-retired_at<3:
                    last_job=int(match[2])
                    if progress is None and a.seconds>40 and now-last_feed>=5:
                        t.check()
                        kernel_write.flush(); os.fsync(kernel_write.fileno())
                        w.integer(KEEPALIVE)
                        last_feed=now
                        print(json.dumps({'event':'gpu-retirement-renewal','job':last_job}),flush=True)
        if time.monotonic()>deadline:
            stop()
            raise RuntimeError('bounded command exceeded deadline; watchdog remains armed')
        time.sleep(0.2)
    relay()
    log.flush();os.fsync(log.fileno())
if kernel_proc is not None:
    if kernel_proc.poll() is not None:
        raise RuntimeError('kernel evidence stream stopped; watchdog remains armed')
    os.killpg(kernel_proc.pid,signal.SIGTERM); kernel_proc.wait(timeout=3)
    kernel_read.close()
    kernel_write.flush(); os.fsync(kernel_write.fileno()); kernel_write.close()
t.check()
# Catch loss that races process exit; printk is evidence, not the health gate.
if progress is not None:
    try:
        progress.poll(time.monotonic())
    except (OSError, RuntimeError, ValueError) as error:
        progress_error = str(error)
        r['progress_error'] = progress_error
r.update(returncode=proc.returncode, thermal=t.summary())
(a.output/'result.json').write_text(json.dumps(r,indent=2)+'\n')
subprocess.run(['dmesg','--color=never'],stdout=(a.output/'dmesg.log').open('w'),check=True,timeout=3)
# Verbose firmware traces can evict the initialization and first fault from
# dmesg. Retain a bounded journal from this boot before disarming the guard.
with (a.output/'kernel-journal.log').open('w') as journal:
    try:
        subprocess.run(['journalctl', '-k', '--boot='+r['boot_id'].replace('-', ''),
                        '--no-pager', '--output=short-monotonic', '--lines=50000'],
                       stdout=journal, check=True, timeout=5)
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
        # The continuous kernel stream and final dmesg are already retained.
        # Preserve the failed optional query explicitly, including partial output.
        r['journal_collection_error'] = str(error)
        (a.output/'journal-collection-error.json').write_text(json.dumps({'error':str(error)})+'\n')
        (a.output/'result.json').write_text(json.dumps(r,indent=2)+'\n')
    journal.flush(); os.fsync(journal.fileno())
os.sync()
final_taint=int(Path('/proc/sys/kernel/tainted').read_text())
final_dmesg=(a.output/'dmesg.log').read_text()
new_dmesg=final_dmesg[len(initial_dmesg):] if final_dmesg.startswith(initial_dmesg) else final_dmesg
# The ring can wrap during verbose GPU bring-up. Retain the launcher's
# continuous stream as well, so an earlier fault cannot vanish at teardown.
live=a.output/'kernel-live.log'
if live.exists():
    new_dmesg += live.read_text()
if ('sealed ' in new_dmesg and 'firmware fault snapshot' in new_dmesg and
        not list(a.output.glob('g16-fault-*.bin'))):
    # Copy the immutable kernel-owned snapshot before a fault reboot. This
    # does not read GPU registers or renew the watchdog. The collector also
    # requires this live guard and fresh six-channel thermal evidence.
    t.check()
    with (a.output/'fault-collection.log').open('w') as diagnostics:
        subprocess.run(['python3', '/root/g16-fault-dump.py', 'collect',
                        '--guard-output', str(a.output), '--allow-missing'],
                       stdout=diagnostics, stderr=subprocess.STDOUT, timeout=5)
        diagnostics.flush(); os.fsync(diagnostics.fileno())
    os.sync()
faults=['Internal error:', 'BUG:', 'Oops:', 'Kernel panic', 'soft lockup', 'hard LOCKUP', 'ASC failed to stop', 'GPU retirement unproven', 'first runtime error', 'firmware crashed']
if progress_error or (final_taint & ~initial_taint & ((1 << 7) | (1 << 9) | (1 << 14))) or any(f in new_dmesg for f in faults):
    print('Kernel fault detected; requesting reboot with watchdog still armed',flush=True)
    subprocess.run(['systemctl','reboot'],check=True,timeout=5)
    raise RuntimeError('kernel fault during guarded command; watchdog remains armed')
t.close()
w.disarm()
print(json.dumps(r,indent=2),flush=True)
sys.exit(proc.returncode)
