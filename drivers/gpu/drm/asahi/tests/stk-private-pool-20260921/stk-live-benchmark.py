#!/usr/bin/env python3
"""Guarded Black Forest benchmark on the current 1080p external display.

Usage through gpu-experiment.py: python3 /root/stk-live-benchmark.py {output}
Uses a private copy of the user's settings, 2x render scale (3840x2160),
fullscreen, and no game FPS cap. The built-in race profiler excludes menus.
Retains its CSV, settings, renderer maps, loading/racing screenshots and monitor state.
--perf-race samples all CPUs for 15 seconds after the 65-second race screenshot;
it includes kernel command preparation, and is diagnostic rather than a clean FPS run.
"""
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import signal
import subprocess
import sys
import time

out = Path(sys.argv[1])
renderer = sys.argv[2] if len(sys.argv)>2 else 'opengl'
assert renderer in ('opengl','vulkan')
spec = importlib.util.spec_from_file_location('fw', '/root/firmware-snapshot.py')
fw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fw)
fw.check_guard(out, Path('/proc/sys/kernel/random/boot_id').read_text().strip())
user = pwd.getpwnam('e')
pid = subprocess.check_output(['pgrep', '-u', str(user.pw_uid), '-x', 'Hyprland'], text=True).split()[0]
env = dict(v.decode().split('=', 1) for v in Path('/proc', pid, 'environ').read_bytes().split(b'\0') if b'=' in v)
env['HYPRLAND_INSTANCE_SIGNATURE'] = next(Path('/run/user/1000/hypr').iterdir()).name
env.update(HOME=user.pw_dir, USER='e', LOGNAME='e', XDG_RUNTIME_DIR='/run/user/1000')
env['WAYLAND_DISPLAY'] = next(p.name for p in Path('/run/user/1000').glob('wayland-*') if p.is_socket())
for key in ['NOTIFY_SOCKET', 'INVOCATION_ID', 'JOURNAL_STREAM', 'SYSTEMD_EXEC_PID']:
    env.pop(key, None)
def ctl(*args):
    return subprocess.check_output(['runuser', '-u', 'e', '--', 'hyprctl', *args], env=env, text=True, timeout=5)
monitors = json.loads(ctl('-j', 'monitors'))
(out/'monitors-before.json').write_text(json.dumps(monitors, indent=2))
monitor = next(m for m in monitors if m['name'] == 'DP-1')
assert (monitor['width'], monitor['height'], monitor['scale']) == (1920, 1080, 1)
focus = next(m['name'] for m in monitors if m['focused'])
assert subprocess.run(['pgrep', '-x', 'supertuxkart'], capture_output=True).returncode != 0
scratch = Path(user.pw_dir)/'.cache/m4-stk-benchmark'/out.name
config = scratch/'config-0.10'
config.mkdir(parents=True)
for f in (Path(user.pw_dir)/'.config/supertuxkart/config-0.10').glob('*.xml'):
    shutil.copy2(f, config/f.name)
shutil.copy2(config/'config.xml', out/'original-config.xml')
xml = (config/'config.xml').read_text()
for k, v in {'swap-interval-vsync':'0', 'max_fps':'1000', 'scale_rtts_factor':'2.000000',
             'non_ge_fullscreen_desktop':'true', 'show_fps':'true'}.items():
    xml, n = re.subn(r'\b'+k+r'="[^"]*"', k+'="'+v+'"', xml)
    assert n == 1, (k,n)
(config/'config.xml').write_text(xml)
shutil.copy2(config/'config.xml', out/'requested-config.xml')
for f in [scratch.parent, scratch, config, *config.iterdir()]:
    os.chown(f, user.pw_uid, user.pw_gid)
env.update(SUPERTUXKART_SAVEDIR=str(scratch), SDL_VIDEODRIVER='x11', DISPLAY=':0',
           SDL_VIDEO_HIGHDPI_DISABLED='1',
           SDL_VIDEO_FULLSCREEN_DISPLAY=str(monitors.index(monitor)),
           SDL_VIDEO_WINDOW_POS=f"{monitor['x']},{monitor['y']}")
probe_seconds = None
capture_frames = '300-302'
for arg in sys.argv[3:]:
    if arg.startswith('--env='):
        k,v=arg[6:].split('=',1)
        assert k.startswith(('ASAHI_','AGX_','HK_','MESA_','VK_','GFXRECON_','ZINK_','LD_'))
        env[k]=v
    elif arg.startswith('--capture-frames='):
        capture_frames=arg.split('=',1)[1]
        assert re.fullmatch(r'[0-9]+-[0-9]+', capture_frames)
    elif arg.startswith('--probe-seconds='):
        probe_seconds=int(arg.split('=',1)[1])
        assert 1 <= probe_seconds <= 120
argv=['/usr/bin/supertuxkart','--benchmark','--fullscreen','--screensize=1920x1080','--render-driver='+renderer]
if '--capture' in sys.argv:
    env.update(VK_LAYER_PATH='/opt/gfxreconstruct/share/vulkan/explicit_layer.d',
        VK_INSTANCE_LAYERS='VK_LAYER_LUNARG_gfxreconstruct',
        GFXRECON_CAPTURE_FILE=str(scratch/'race.gfxr'), GFXRECON_CAPTURE_FILE_TIMESTAMP='false',
        GFXRECON_CAPTURE_FILE_FLUSH='true', GFXRECON_CAPTURE_FRAMES=capture_frames)
if '--no-texture-compression' in sys.argv:
    argv.append('--disable-texture-compression')
(out/'launch.json').write_text(json.dumps(dict(argv=argv, internal_resolution=[3840,2160],
    display_resolution=[1920,1080], environment={k:v for k,v in env.items() if k.startswith(
    ('SDL_','VK_','MESA_','LD_','GALLIUM_','ASAHI_','HK_','SUPERTUX'))}), indent=2))
child=None
perf=None
perf_log=None
def interrupted(signum, frame):
    raise InterruptedError(f'signal {signum}')
signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)
try:
    subprocess.run(['systemctl','--user','--machine=e@.host','stop','xdg-desktop-portal-hyprland.service'],check=True,timeout=10)
    subprocess.run(['pkill','-TERM','-u','1000','-x','quickshell'],check=False)
    time.sleep(2)
    (out/'memory-before.txt').write_text(Path('/proc/meminfo').read_text())
    ctl('eval','hl.dispatch(hl.dsp.focus({ monitor = "DP-1" }))')
    with (out/'supertuxkart.log').open('w') as log:
        child=subprocess.Popen(['runuser','-u','e','--',*argv],env=env,stdout=log,
            stderr=subprocess.STDOUT,start_new_session=True)
        started=time.monotonic()
        captured=False
        late_captured=False
        while child.poll() is None:
            elapsed=time.monotonic()-started
            if elapsed > 1 and not (out/'clients-start.json').exists():
                (out/'clients-start.json').write_text(ctl('-j','clients'))
            text=(out/'supertuxkart.log').read_text(errors='replace')
            if re.search(r'M4 compressed image: (?:7680x4320|5120x3328)', text):
                raise RuntimeError('Unexpected oversized render targets; stop before memory pressure')
            available=int(re.search(r'^MemAvailable:\s+(\d+)',Path('/proc/meminfo').read_text(),re.M)[1])
            with (out/'memory-samples.jsonl').open('a') as mem:
                procs={}
                for p in Path('/proc').glob('[0-9]*'):
                    try:
                        if (p/'comm').read_text().strip()=='supertuxkart':
                            procs[p.name]=(p/'status').read_text()
                    except OSError:pass
                mem.write(json.dumps(dict(seconds=elapsed,meminfo=Path('/proc/meminfo').read_text(),processes=procs))+'\n')
            if available < 2*1024*1024:
                raise RuntimeError('Less than 2 GiB available; stopping benchmark before OOM')
            if elapsed > 250:
                raise TimeoutError('Benchmark did not finish within 250 seconds')
            if probe_seconds and elapsed >= probe_seconds:
                break
            if elapsed > 30 and not captured:
                (out/'clients-racing.json').write_text(ctl('-j','clients'))
                with (out/'race.png').open('wb') as image:
                    subprocess.run(['runuser','-u','e','--','grim','-o','DP-1','-'],env=env,
                                   stdout=image,check=True,timeout=10)
                for p in subprocess.check_output(['pgrep','-x','supertuxkart'],text=True).split():
                    (out/f'{p}-maps.txt').write_text(Path('/proc',p,'maps').read_text())
                captured=True
            if elapsed > 65 and not late_captured:
                with (out/'race-late.png').open('wb') as image:
                    subprocess.run(['runuser','-u','e','--','grim','-o','DP-1','-'],env=env,
                                   stdout=image,check=True,timeout=10)
                late_captured=True
                if '--perf-race' in sys.argv:
                    perf_env={**os.environ, 'LD_LIBRARY_PATH':'/opt/m4-profiling/usr/lib',
                              'DEBUGINFOD_URLS':''}
                    perf_argv=['/opt/m4-profiling/usr/bin/perf','record','-a',
                               '-e','cpu-clock','-F','99','--call-graph','fp',
                               '-o',str(out/'race-perf.data'),'--','sleep','15']
                    (out/'race-perf.json').write_text(json.dumps(dict(
                        argv=perf_argv, start_seconds=elapsed, scope='All CPUs and clients; includes kernel'),indent=2))
                    perf_log=(out/'race-perf.log').open('w')
                    perf=subprocess.Popen(perf_argv,env=perf_env,stdout=perf_log,
                                          stderr=subprocess.STDOUT,start_new_session=True)
            time.sleep(.5)
        assert (probe_seconds and child.poll() is None) or child.returncode==0, child.returncode
finally:
    if perf and perf.poll() is None:
        os.killpg(perf.pid,signal.SIGINT)
        try: perf.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(perf.pid,signal.SIGKILL)
            perf.wait(timeout=3)
    if perf_log:
        perf_log.close()
    if child and child.poll() is None:
        os.killpg(child.pid,signal.SIGTERM)
        try: child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid,signal.SIGKILL)
            child.wait(timeout=3)
    shutil.copytree(config, out/'game-output')
    for trace in scratch.glob('*.gfxr'):
        shutil.copy2(trace,out/trace.name)
    ctl('eval','hl.dispatch(hl.dsp.focus({ monitor = '+json.dumps(focus)+' }))')
    (out/'monitors-after.json').write_text(ctl('-j','monitors'))
    subprocess.run(['systemctl','restart','display-manager'],check=True,timeout=20)
    time.sleep(8)
if probe_seconds:
    print('PASS: bounded STK diagnostic completed; not a benchmark result',flush=True)
    sys.exit(0)
reports=list((out/'game-output').glob('*.perf-report-black_forest.csv'))
assert len(reports)==1, reports
lines=reports[0].read_text().splitlines()
frames, milliseconds = map(int, lines[1].rstrip(',').split(','))
values=next(l for l in lines if l.startswith('Values,')).split(',')
assert list(map(int,values[1:3]))==[1920,1080] and float(values[3])==2 and int(values[4])==1, values
result=dict(frames=frames, seconds=milliseconds/1000, average_fps=frames*1000/milliseconds,
            renderer=renderer,
            internal_resolution=[3840,2160], fullscreen_output=[1920,1080],
            race='Black Forest built-in benchmark', game_metrics=lines[4], menus_excluded=True)
(out/'benchmark-summary.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: race benchmark complete '+json.dumps(result),flush=True)
