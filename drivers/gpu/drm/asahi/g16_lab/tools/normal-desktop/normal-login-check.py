#!/usr/bin/env python3
"""Guarded real SDDM keyboard login; password read from stdin, never saved.

Run on J713 with --output NEW_DIRECTORY. Tests the normal installed session,
captures before/after images and renderer evidence, and leaves the session up.
"""
import argparse
import json
import os
from pathlib import Path
import pwd
import subprocess
import stat
import struct
import sys
import time
sys.path.insert(0, '/root/g16-submit-test')
from experiment_watchdog import ExperimentWatchdog
from experiment_thermals import ExperimentThermals

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--capture-compiler',action='store_true')
p.add_argument('--native-screenshots',action='store_true',help='Read native scanout without compositor GPU readback')
p.add_argument('--existing-greeter',action='store_true',help='Validate the greeter started by normal boot')
a=p.parse_args()
password=sys.stdin.read().rstrip('\n')
assert password
a.output.mkdir(parents=True,exist_ok=False)
w=ExperimentWatchdog()
record={'watchdog':w.arm()}
t=ExperimentThermals(a.output,on_trip=lambda:subprocess.run(
    ['systemctl','--no-block','stop','sddm'],timeout=3))
began=time.monotonic()
def processes(uid, name):
    found=[]
    for path in Path('/proc').glob('[0-9]*'):
        try:
            if path.stat().st_uid==uid and (path/'exe').resolve().name==name:
                found.append(path)
        except (FileNotFoundError,ProcessLookupError):pass
    return found
def wait_for(check):
    while time.monotonic()-began < 45:
        t.check()
        answer=check()
        if answer:return answer
        time.sleep(0.2)
    raise TimeoutError('Normal graphical login did not become ready')
def compositor_env(proc):
    env={k.decode():v.decode() for k,v in (x.split(b'=',1) for x in
         (proc/'environ').read_bytes().split(b'\0') if b'=' in x)}
    runtime=Path('/run/user')/str(proc.stat().st_uid)
    instances=list((runtime/'hypr').glob('*/.socket.sock'))
    assert len(instances)==1, instances
    env.update(XDG_RUNTIME_DIR=str(runtime),HYPRLAND_INSTANCE_SIGNATURE=instances[0].parent.name)
    sockets=[p for p in runtime.glob('wayland-*') if stat.S_ISSOCK(p.stat().st_mode)]
    assert len(sockets)==1, sockets
    env['WAYLAND_DISPLAY']=sockets[0].name
    return env
def screenshot(name, env):
    t.check()
    if a.native_screenshots:
        nodes={p.resolve() for p in Path('/sys/kernel/debug/dri').glob('*/m4_scanout')}
        assert len(nodes)==1, nodes
        raw=nodes.pop().read_bytes()
        magic,width,height,stride,fmt,fb,imported,size=struct.unpack_from('<8I',raw)
        assert magic==0x4246344d and imported==1 and len(raw)==size+32
        assert fmt in (int.from_bytes(b'XR24','little'),int.from_bytes(b'AR24','little'))
        (a.output/(name+'.m4fb')).write_bytes(raw)
    else:
        subprocess.run(['grim','-t','ppm',str(a.output/(name+'.ppm'))],env=env,check=True,timeout=5)
    t.check()

try:
    t.start()
    t.cap_cpu_frequency(1860000)
    if not a.existing_greeter:
        subprocess.run(['systemctl','stop','sddm','user@1000.service'],check=True,timeout=10)
    if a.capture_compiler:
        Path('/run/m4-graphics/capture-compiler').touch(mode=0o600)
    session_started=time.time()
    subprocess.run(['systemctl','start','sddm'],check=True,timeout=15)
    greeter_uid=pwd.getpwnam('sddm').pw_uid
    wait_for(lambda:processes(greeter_uid,'sddm-greeter-qt6'))
    greeter=wait_for(lambda:processes(greeter_uid,'Hyprland'))[0]
    genv=compositor_env(greeter)
    wait_for(lambda:any(c.get('mapped') and c.get('class') == 'sddm-greeter-qt6'
        for c in json.loads(subprocess.check_output(['hyprctl','-j','clients'],env=genv,text=True,timeout=2))))
    time.sleep(0.5)
    screenshot('greeter',genv)
    subprocess.run(['wtype','-M','ctrl','a','-m','ctrl','-','-k','Return'],
                   env=genv,input=password,text=True,check=True,timeout=3)
    password=None
    wait_for(lambda:processes(1000,'quickshell'))
    compositor=wait_for(lambda:processes(1000,'Hyprland'))[-1]
    env=compositor_env(compositor)
    time.sleep(2)
    t.check()
    for proc in [compositor,*processes(1000,'quickshell')]:
        maps=(proc/'maps').read_text()
        assert '/opt/asahi-m4-port/lib/libvulkan_asahi.so' in maps
        (a.output/(proc.name+'-maps.txt')).write_text(maps)
    command=(compositor/'cmdline').read_bytes().replace(b'\0',b' ').decode()
    assert '/usr/local/share/m4-desktop/session.lua' in command, command
    record['compositor_command']=command
    for name,command in [('monitors',['hyprctl','-j','monitors']),('layers',['hyprctl','-j','layers'])]:
        data=subprocess.check_output(command,env=env,text=True,timeout=3)
        (a.output/(name+'.json')).write_text(data)
    layers=(a.output/'layers.json').read_text()
    assert 'omarchy-bar' in layers and 'omarchy-background' in layers
    screenshot('desktop',env)
    subprocess.run(['hyprctl','dispatch','hl.dsp.exec_cmd("foot")'],env=env,check=True,timeout=3)
    wait_for(lambda:any(c.get('mapped') and c.get('class') == 'foot'
        for c in json.loads(subprocess.check_output(['hyprctl','-j','clients'],env=env,text=True,timeout=2))))
    time.sleep(1)
    t.check()
    screenshot('desktop-terminal',env)
    for _ in range(8):
        time.sleep(0.5)
        t.check()
    screenshot('desktop-terminal-later',env)
    login_log=subprocess.check_output(['journalctl','-b','--since','@'+str(session_started),
        '--no-pager'],text=True,timeout=3)
    (a.output/'login-journal.log').write_text(login_log)
    for failure in ('no grounded AGX3 encoding', 'AGX backend rejected',
                    'terminated abnormally', 'Kernel panic', 'GPU timeout', 'DEVICE LOST'):
        assert failure not in login_log, failure
    record['passed']=True
except BaseException as error:
    record.update(passed=False,error=repr(error))
    subprocess.run(['systemctl','--no-block','stop','sddm'],timeout=3)
    raise
finally:
    if a.capture_compiler:
        Path('/run/m4-graphics/capture-compiler').unlink(missing_ok=True)
    if not record.get('passed'):
        for node in Path('/sys/class/devcoredump').glob('devcd*/data'):
            try:
                with node.open('rb',buffering=0) as source:
                    raw=source.read(16*1024*1024+1)
                if raw.startswith(b'M4FWD001') and len(raw)<=16*1024*1024:
                    (a.output/(node.parent.name+'.bin')).write_bytes(raw)
            except OSError:
                pass
    record['thermal']=t.summary()
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    with (a.output/'journal.log').open('w') as f:
        subprocess.run(['journalctl','-b','--since','-2min','--no-pager'],stdout=f,timeout=3)
    t.close()
    if record.get('passed'):
        for cap in record['thermal'].get('cpu_frequency_caps',[]):
            (Path('/sys/devices/system/cpu/cpufreq')/cap['policy']/'scaling_max_freq').write_text(str(cap['old_max_khz']))
        record['test_cpu_caps_restored']=True
        (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    os.sync()
w.disarm()
print('PASS: real SDDM keyboard login into normal accelerated Omarchy',flush=True)
