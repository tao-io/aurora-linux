#!/usr/bin/env python3
"""Small screenshot/keyboard bridge for an existing desktop, under gpu-experiment.

Server: desktop-ui.py serve GUARD_OUTPUT
Client: desktop-ui.py send GUARD_OUTPUT '{"action":"snapshot","name":"menu"}'
Actions: snapshot, key (150 ms hold), keys (wtype argv), paste (text), finish.
The server owns no watchdog; its ancestor guard requires live GPU retirement and
thermal evidence. Saves native scanout after every action. Stops any game started
during this session before returning. The caller visually reviews each image.
"""
import argparse, importlib.util, json, os, signal, socket, stat, struct, subprocess, time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('mode',choices=['serve','send']);p.add_argument('output',type=Path);p.add_argument('request',nargs='?')
p.add_argument('--seconds',type=int,default=180)
a=p.parse_args();sockpath=a.output/'ui.sock'
if a.mode=='send':
    request=json.loads(a.request)
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(15);s.connect(str(sockpath));s.sendall(json.dumps(request).encode()+b'\n')
        print(s.makefile('r').readline(),end='')
    raise SystemExit
spec=importlib.util.spec_from_file_location('fw','/root/firmware-snapshot.py');fw=importlib.util.module_from_spec(spec);spec.loader.exec_module(fw)
boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip()
def check():fw.check_guard(a.output,boot)
def games():
    result=set()
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            if proc.stat().st_uid==1000 and (proc/'comm').read_text().strip()=='supertuxkart':
                result.add(int(proc.name))
        except (FileNotFoundError, ProcessLookupError):pass
    return result
check();original=games()
def interrupted(signum, frame):raise SystemExit('UI session interrupted by guard')
signal.signal(signal.SIGTERM,interrupted)
signal.signal(signal.SIGINT,interrupted)
deadline=time.monotonic()+a.seconds
procs=[]
for proc in Path('/proc').glob('[0-9]*'):
    try:
        if proc.stat().st_uid==1000 and (proc/'exe').resolve().name=='Hyprland':procs.append(proc)
    except (FileNotFoundError, ProcessLookupError):pass
assert len(procs)==1,procs
proc=procs[0]
env={k.decode():v.decode() for k,v in (x.split(b'=',1) for x in (proc/'environ').read_bytes().split(b'\0') if b'=' in x)}
# The tools do no GL work; do not inherit diagnostic preload settings.
for key in ['LD_PRELOAD','TRACE_FILE']:env.pop(key,None)
runtime=Path('/run/user/1000');instances=list((runtime/'hypr').glob('*/.socket.sock'))
assert len(instances)==1
env['XDG_RUNTIME_DIR']=str(runtime);env['HYPRLAND_INSTANCE_SIGNATURE']=instances[0].parent.name
wayland=[p for p in runtime.glob('wayland-*') if stat.S_ISSOCK(p.stat().st_mode)];assert len(wayland)==1
env['WAYLAND_DISPLAY']=wayland[0].name
seq=0
def snapshot(name):
    assert name.replace('-','').replace('_','').isalnum()
    nodes={p.resolve() for p in Path('/sys/kernel/debug/dri').glob('*/m4_scanout')};assert len(nodes)==1
    raw=nodes.pop().read_bytes();magic,width,height,stride,fmt,fb,imported,size=struct.unpack_from('<8I',raw)
    assert magic==0x4246344d and imported==1 and len(raw)==size+32
    path=a.output/f'{seq:03d}-{name}.m4fb';path.write_bytes(raw)
    return {'image':str(path),'framebuffer':fb,'width':width,'height':height}
snapshot('initial')
with socket.socket(socket.AF_UNIX) as server:
    server.bind(str(sockpath));os.chmod(sockpath,0o600);server.listen(1);server.settimeout(.5)
    print('UI ready: '+str(sockpath),flush=True)
    try:
        while True:
            if time.monotonic()>deadline:raise TimeoutError('UI deadline reached')
            check()
            try:conn,_=server.accept()
            except socket.timeout:continue
            with conn:
                conn.settimeout(3);raw=conn.makefile('rb').readline(65537);assert len(raw)<=65536
                request=json.loads(raw);seq+=1
                (a.output/f'{seq:03d}-request.json').write_text(json.dumps(request,indent=2)+'\n')
                action=request['action'];check()
                if action=='key':
                    subprocess.run(['wtype','-P',request['key'],'-s','150','-p',request['key']],env=env,check=True,timeout=5)
                elif action=='keys':
                    subprocess.run(['wtype','-d','100',*request['args']],env=env,check=True,timeout=5)
                elif action=='paste':
                    subprocess.run(['wl-copy','--type','text/plain;charset=utf-8','--',request['text']],env=env,check=True,timeout=3)
                    subprocess.run(['wtype','-M','ctrl','-M','shift','v','-m','shift','-m','ctrl'],env=env,check=True,timeout=3)
                else:assert action in ('snapshot','finish'),action
                time.sleep(.3);check();result=snapshot(request.get('name',action))
                conn.sendall(json.dumps(result).encode()+b'\n')
                if action=='finish':break
    finally:
        for pid in games()-original:
            try:os.kill(pid,signal.SIGTERM)
            except ProcessLookupError:pass
        deadline=time.monotonic()+3
        while games()-original and time.monotonic()<deadline:time.sleep(.1)
        for pid in games()-original:
            try:os.kill(pid,signal.SIGKILL)
            except ProcessLookupError:pass
        sockpath.unlink(missing_ok=True)
check();print('PASS: bounded desktop UI session closed',flush=True)
