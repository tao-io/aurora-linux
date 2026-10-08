"""Prepared only: owner performs TahoeResearch boot selection; never reboots either host.
Run under rig-lock AFTER owner release and cable reconnection; result is RAM only, READ-ONLY; the write ladder is a separate owner action.
Do not run the old run-d3.py: its closure selects #37.
"""
from pathlib import Path
import os,subprocess as sp,time,json,hashlib,signal
B=Path(__file__).resolve().parent;A=B/'ram';M=json.loads((B/'manifest.json').read_text())
for k in ('J613_RIG_LOCK_HELD','J613_OWNER_RELEASED','J613_CABLE_CONNECTED'):assert os.environ.get(k)=='1',k
A.mkdir(exist_ok=False);log=(A/'cycle.log').open('w',buffering=1)
def q(h,c,timeout=20,check=True):
 r=sp.run(['ssh','-o','BatchMode=yes','-o','ConnectTimeout=4','-o','ServerAliveInterval=2','-o','ServerAliveCountMax=2',h,'set -eu; '+c],capture_output=True,timeout=timeout)
 log.write(h+' '+c+'\n'+r.stdout.decode(errors='replace')+r.stderr.decode(errors='replace'))
 if check:assert r.returncode==0,c
 return r
inventory=sp.run(['limactl','shell','neo-build','--','ps','-eo','pid,args'],capture_output=True,text=True,timeout=20);assert inventory.returncode==0;log.write(inventory.stdout)
source=B/'stage2-d3h-nvram-write.bin';assert hashlib.sha256(source.read_bytes()).hexdigest()==M['stage_sha256']
q('rubra','test "$(sudo asahi-bless --get-boot)" = TahoeResearch')
q('swallow','case $(uname -r) in 7.1.12-aurora-neo-0929c|7.1.12-aurora-neo-gpu1007b) ;; *) exit 1;; esac; test -e /sys/bus/spmi/drivers/sn201202x/1-0c; test ! -e /dev/ttyACM0')
q('rubra','test "$(uname -r)" = 7.1.9-eryk-m4j613-j613-d3h; c=$(cat /sys/class/power_supply/macsmc-battery/capacity); a=$(cat /sys/class/power_supply/macsmc-ac/online); test "$c" -gt 30 || test "$a" = 1')
# switch-root can load the root module: require candidate installed before boot.
q('rubra','test "$(sha256sum /lib/modules/'+M['release']+'/kernel/drivers/spi/spi-apple-qspi.ko | cut -d" " -f1)" = '+M['qspi_module_sha256'])
# No process/session mutations: owner must have logged out before this observer.
q('rubra','! pgrep -u satchlj -x Hyprland; systemctl is-active --quiet sddm')
remote='/home/satchlj/Work/rubra-proxy/stage2-d3h-nvram-write-20261007.bin'
sp.run(['scp',str(source),'swallow:'+remote],check=True)
q('swallow','test "$(sha256sum '+remote+' | cut -d" " -f1)" = '+M['stage_sha256'])
watch=sp.Popen(['bash',str(B.parent/'j613-d3-qualification-20261007/watch-rig.sh')],stdout=(A/'watch-rig.log').open('w'),stderr=sp.STDOUT,env=dict(os.environ,RUBRA_TEST_LIMIT='1200'))
passed=False
try:
 q('swallow','echo 1-0c | sudo tee /sys/bus/spmi/drivers/sn201202x/unbind; test ! -e /sys/bus/spmi/drivers/sn201202x/1-0c')
 print('READY: owner may now reboot/power-select TahoeResearch; default stays unchanged. Waiting for proxy (300 s).',flush=True)
 end=time.monotonic()+300
 while time.monotonic()<end:
  assert 'ALERT' not in (A/'watch-rig.log').read_text()
  if q('swallow','test -e /dev/ttyACM0',check=False).returncode==0:break
  time.sleep(.5)
 else:raise RuntimeError('Proxy absent; no automatic recovery')
 r=q('swallow','cd ~/Work/m3-tahoe-research/local/m1n1/proxyclient && M1N1DEVICE=/dev/ttyACM0 timeout 120 /home/satchlj/Work/m3-tahoe-research/local/venv/bin/python tools/chainload.py -r '+remote,timeout=135,check=False)
 (A/'chainload.txt').write_bytes(r.stdout+r.stderr)
 end=time.monotonic()+200
 while time.monotonic()<end:
  assert 'ALERT' not in (A/'watch-rig.log').read_text()
  r=q('rubra','test "$(uname -r)" = 7.1.9-eryk-m4j613-j613-d3h; test "$(stat -c %a /sys/module/spi_apple_qspi/parameters/read_only)" = 644; test "$(sudo od -An -v -tx1 /sys/module/spi_apple_qspi/notes/.note.gnu.build-id | tr -d " \n")" = '+M['qspi_build_id_note_hex'],check=False)
  if r.returncode==0:break
  time.sleep(2)
 else:raise RuntimeError('RAM candidate SSH absent')
 sp.run(['bash',str(B/'check-nvram.sh'),str(A)],check=True)
 time.sleep(15)
 sp.run(['bash',str(B/'check-nvram.sh'),str(A)],check=True)
 sp.run(['bash',str(B/'check-haptics-nofinger.sh'),str(A)],check=True)
 passed=True
 (A/'result.json').write_text(json.dumps(dict(passed=True,stage_sha256=M['stage_sha256'],ram_only=True,installed=False,human_nvram_reads=False,next_boot_write_tested=False),indent=2)+'\n')
 print('RAM machine checks PASS; remaining on D3h, read-only. Write qualification is separate; do not arm read_only here.',flush=True)
finally:
 if passed:
  q('swallow','echo 1-0c | sudo tee /sys/bus/spmi/drivers/sn201202x/bind; test -e /sys/bus/spmi/drivers/sn201202x/1-0c')
  watch.terminate();watch.wait(timeout=10)
 else:
  (A/'result.json').write_text(json.dumps(dict(passed=False,stage_sha256=M['stage_sha256'],lock_retained=True,owner_recovery_required=True),indent=2)+'\n')
  print('STOP: owner recovery required; lock/watcher retained. No #37 selection or Swallow reboot.',flush=True)
  os.kill(os.getpid(),signal.SIGSTOP)
 log.close()
