#!/usr/bin/env python3
"""Run the retained M4 cleanup gates through the existing watchdog/thermal guard.

Run from the workspace root. No deployment, reboot or new CTS selection is done.
Every step records installed-module/boot identity and preserves its complete
experiment directory. A failed guard, pixel mismatch or CTS result stops the
suite. This is focused regression evidence, not broad conformance coverage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--name', required=True)
    p.add_argument('--module-sha256', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--steps', nargs='+', choices=['replay', 'cts', 'desktop'],
                   default=['replay', 'cts', 'desktop'])
    a = p.parse_args()
    if not re.fullmatch(r'[a-z0-9-]+', a.name):
        p.error('name must be a lowercase experiment label')
    if not re.fullmatch(r'[0-9a-f]{64}', a.module_sha256):
        p.error('a complete installed module SHA256 is required')
    a.output.mkdir(parents=True, exist_ok=False)
    runner = '/root/cts-bulk/cleanup-notification-cts-3e5362b58525'
    prefix = '/opt/asahi-m3m4-5f47e5ec5b6'
    commands = {
        'replay': (40, 'PASS: Vulkan replay process completed', [
            'python3', '/root/factorio-replay.py', '{output}', '--icd', prefix+'/asahi.json',
            '--mesa-prefix', prefix]),
        'cts': (40, '^CTS_BATCH_COMPLETE:', [
            'python3', runner+'/cts-sample.py', 'batch', '--backend', 'deqp-runner',
            '--guard-output', '{output}', '--binary', '/root/cts-1.4.6.2/deqp-vk',
            '--cases', runner+'/cases.txt', '--deqp-runner', runner+'/deqp-runner',
            '--count', '12', '--jobs', '1', '--tests-per-group', '1',
            '--runner-seconds', '20', '--case-timeout', '10', '--renderer', 'm4',
            '--memory-floor-mib', '1536', '--env', 'VK_DRIVER_FILES='+prefix+'/asahi.json',
            '--env', 'LD_LIBRARY_PATH='+prefix+'/lib']),
        'desktop': (30, 'PASS: bounded real application observation completed', [
            'python3', '/root/app-observe.py', '--guard-output', '{output}', '--seconds', '6',
            '--', '/usr/bin/foot', '--working-directory=/home/e', '-T', 'Driver cleanup validation',
            '/bin/sh', '-c', 'printf "M4 driver cleanup: desktop validation\\n"; sleep 15']),
    }
    records = []
    boot = None
    for step in a.steps:
        identity_script = '''import hashlib,json,os,pathlib,subprocess
p=pathlib.Path('/lib/modules')/os.uname().release/'kernel/drivers/gpu/drm/asahi/asahi.ko'
s=subprocess.run(['systemctl','is-active','m4-graphics','display-manager'],capture_output=True,text=True)
assert s.returncode==0,s.stdout
print(json.dumps(dict(boot=pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip(),module=hashlib.sha256(p.read_bytes()).hexdigest(),release=os.uname().release)))
'''
        import shlex
        identity = json.loads(subprocess.check_output(['ssh', '-o', 'ConnectTimeout=10',
            'root@omarchy-m4', shlex.join(['python3', '-c', identity_script])], text=True))
        if identity['module'] != a.module_sha256:
            raise RuntimeError(('unexpected installed module', identity))
        if boot is None:
            boot = identity['boot']
        if boot != identity['boot']:
            raise RuntimeError('boot changed during suite')
        (a.output/(step+'-identity.json')).write_text(json.dumps(identity, indent=2)+'\n')
        seconds, expect, command = commands[step]
        argv = ['python3', 'tools/gpu-experiment.py', '--name', a.name+'-'+step,
            '--mesa-source', 'mesa/m3m4-merged', '--hypothesis',
            'Representation cleanup preserves the pinned '+step+' regression gate',
            '--expect', expect, '--seconds', str(seconds), '--thermal-stop-mc', '120000',
            '--cpu-max-khz', '4104000', '--'] + command
        with (a.output/(step+'.log')).open('w') as log:
            result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
        raw = (a.output/(step+'.log')).read_text()
        if result.returncode != 0:
            raise RuntimeError('guard failed; inspect '+str(a.output/(step+'.log')))
        report = json.loads(raw[raw.index('\n{'):])
        if report['verdict'] != 'pass':
            raise RuntimeError(report)
        evidence = Path(report['directory'])
        guard = json.loads((evidence/'remote/result.json').read_text())
        if guard['boot_id'] != boot or guard['watchdog']['timeout_seconds'] != 60:
            raise RuntimeError(('guard identity/watchdog', guard))
        item = dict(step=step, evidence=str(evidence.relative_to(Path.cwd())),
            peak_c=report['observations']['peak_temperature_c'])
        if step == 'replay':
            sha = hashlib.sha256((evidence/'remote/vulkan-replay/screenshot_frame_2.png').read_bytes()).hexdigest()
            item['sha256'] = sha
            if sha != '88594c10407e243eb0617860b8f2abc1f06c0844ec67ddba3900875d7e36796e':
                raise RuntimeError('replay differs from retained reference: '+sha)
        elif step == 'cts':
            cases = json.loads((evidence/'remote/cts/results.json').read_text())
            item['cts'] = [[x['eligibility']['catalog_index'],x['case'],x['status']] for x in cases]
            expected = [1563743,1563747,1563749,1563703,1563707,1563709,474877,474875,474869,474873,2155251,2155283]
            if sorted(x[0] for x in item['cts']) != sorted(expected) or any(x[2]!='Pass' for x in item['cts']):
                raise RuntimeError(('CTS coverage or result changed',item['cts']))
        else:
            image = a.output/'desktop.png'
            subprocess.run(['python3', 'tools/scanout-image.py',
                str(evidence/'remote/application/screen-6.m4fb'), str(image)], check=True)
            item['image'] = str(image)
            item['visual_review'] = 'pending; inspect image separately'
        records.append(item)
        (a.output/'result.json').write_text(json.dumps(dict(boot_id=boot,
            module_sha256=a.module_sha256,steps=records),indent=2)+'\n')
        print('PASS:',step,item['evidence'],flush=True)


if __name__ == '__main__':
    main()
