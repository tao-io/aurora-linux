#!/usr/bin/env python3
"""Read-only GPU experiment identity, including explicitly selected private ICDs.

Accept the experiment argv as positional arguments. No library is loaded and
no GPU or physical-memory access is performed. Environment assignments in argv
are declarations, not proof that an opaque wrapper ultimately used that ICD.
"""
import hashlib,json,os,sys
from pathlib import Path


def selected_files(argv):
    env={}
    for item in argv:
        if '=' in item:
            key,value=item.split('=',1)
            if key in {'VK_DRIVER_FILES','VK_ICD_FILENAMES','LD_PRELOAD','LD_LIBRARY_PATH'}:
                env[key]=value
    files=set()
    unresolved=[]
    for key in ['VK_DRIVER_FILES','VK_ICD_FILENAMES']:
        for name in filter(None,env.get(key,'').split(':')):
            path=Path(name)
            if not path.is_absolute() or not path.is_file():
                unresolved.append(dict(selector=key,path=name,reason='missing or relative ICD'))
                continue
            files.add(path)
            if path.stat().st_size>1024*1024:
                raise ValueError('Oversized ICD identity input')
            library=json.loads(path.read_text())['ICD']['library_path']
            lib=Path(library)
            if not lib.is_absolute() and '/' in library:
                lib=path.parent/lib
            elif not lib.is_absolute():
                candidates=[Path(d)/lib for d in env.get('LD_LIBRARY_PATH','').split(':') if d]
                lib=next((p for p in candidates if p.is_absolute() and p.is_file()),lib)
            if lib.is_absolute() and lib.is_file():files.add(lib)
            else:unresolved.append(dict(selector=key,path=library,reason='library search not resolved; no loading attempted'))
    for name in filter(None,env.get('LD_PRELOAD','').replace(' ',':').split(':')):
        p=Path(name)
        if p.is_absolute() and p.is_file():files.add(p)
        else:unresolved.append(dict(selector='LD_PRELOAD',path=name,reason='not an existing absolute path'))
    for name in filter(None,env.get('LD_LIBRARY_PATH','').split(':')):
        directory=Path(name)
        if directory.is_absolute() and directory.is_dir():
            for pattern in ['libvulkan_asahi.so','libgallium-*.so','libEGL_mesa.so*','libgbm.so*']:
                files.update(p for p in directory.glob(pattern) if p.is_file())
    return env,files,unresolved


def collect(argv):
    files=[
        '/opt/asahi-m4-port/lib/libvulkan_asahi.so','/root/guarded-command.py','/root/completion_progress.py','/root/idle_work_progress.py',
        '/opt/asahi-m4-port/lib/libgallium-26.1.4.so','/root/run-m4-gl.py',
        '/root/firmware-snapshot.py','/root/g16-fault-dump.py',
        '/lib/modules/'+os.uname().release+'/kernel/drivers/gpu/drm/asahi/asahi.ko',
        '/root/g16-submit-test/experiment_watchdog.py','/root/g16-submit-test/experiment_thermals.py',
    ]+argv
    env,selected,unresolved=selected_files(argv)
    files=[Path(n) for n in files if Path(n).is_absolute() and Path(n).is_file()]+list(selected)
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(files))}
    dt=Path('/sys/firmware/devicetree/base')
    boot={}
    for p in (dt/'chosen').glob('*version*'):
        if p.is_file():boot[p.name]=p.read_bytes()[:4096].rstrip(b'\0').decode('utf-8','replace')
    board={n:(dt/n).read_bytes().rstrip(b'\0').decode('utf-8','replace') for n in ['compatible','model'] if (dt/n).is_file()}
    display_status={str(p):p.read_text() for p in {p.resolve() for p in Path('/sys/kernel/debug/dri').glob('*/m4_status')}}
    return dict(boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        uptime_seconds=float(Path('/proc/uptime').read_text().split()[0]),kernel=os.uname().release,
        files=hashes,display_status=display_status,board=board,boot_versions=boot,
        declared_graphics_environment=env,unresolved_graphics_inputs=unresolved)

if __name__=='__main__':print(json.dumps(collect(sys.argv[1:])))
