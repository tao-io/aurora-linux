# SSH watchdog

`ssh-watchdog.py` implements the user's 2026-09-12 request for a persistent
60-second hardware watchdog renewed over SSH every ten seconds. The target
broker opens `/dev/watchdog0` once and verifies the Apple identity and timeout.
Only a received SSH ping feeds an idle watchdog; no target timer feeds it.

Deploy the script to `/root/ssh-watchdog.py`, and install this target service:

```ini
[Unit]
Description=M4 watchdog owned by SSH heartbeat and guarded experiments
[Service]
Type=exec
ExecStart=/usr/bin/python3 /root/ssh-watchdog.py broker
Restart=no
```

Save as `/etc/systemd/system/m4-ssh-watchdog.service`, then daemon-reload.
Run `python3 tools/ssh-watchdog.py heartbeat` as a persistent host process with
stdin disconnected and stdout/stderr retained. This invokes SSH every ten
seconds, starts the broker if needed, and pings it. Inspect with
`ssh root@omarchy-m4 python3 /root/ssh-watchdog.py status`. The current host
PID and JSONL log are in `_provenance/watchdog-ssh-20260912/`.

The broker uses a root-only Unix socket and SCM_RIGHTS to lend the actual
watchdog descriptor to `ExperimentWatchdog`. Existing guards therefore retain
their ioctl checks, thermal/progress renewal rules, and descriptor-based
firmware snapshot authorization. SSH pings during a lease report `experiment`
without renewing. Successful guard completion returns ownership to idle after
saving evidence. An abandoned connection latches `failed`: SSH cannot renew
or reacquire it, and the hardware watchdog expires. Broker failure likewise
leaves the watchdog armed; `/run/m4-ssh-watchdog.started` prevents a same-boot
restart from masking that failure. Reboot clears `/run` and permits recovery.

Losing the host/SSH connection for 60 seconds intentionally resets the target.
An idle compositor-only stall that leaves SSH responsive is not detected by
an SSH heartbeat; GPU experiments still need the bounded guarded runner.
Stopping the host heartbeat alone leaves recovery armed. Do not stop it just
to end an experiment; release the experiment lease normally instead.

Validation: verified a 60-second ioctl timeout, repeated idle SSH feeds ten
seconds apart, no feed during an exclusive lease, and idle resumption after
release. RGBA32 hardware oracle passed all 36,864 pixel checks under the leased
guard, including fresh six-channel thermal samples. Forced expiry was not
tested on this boot.


### Boost-aware frequency ceilings (2026-09-28)

The thermal guard reads `cpuinfo_max_freq` as well as the ordinary OPP list.
Linux excludes boost entries from `scaling_available_frequencies`; on J713,
boost enabled reports a 4,464,000 kHz hardware maximum while the ordinary list
ends at 4,104,000. A requested 4,104,000 ceiling therefore requires an explicit
QoS write and its existing asynchronous completion check. Skipping that write
formerly made the guard trip before replay, followed by watchdog recovery.

Fifteen host thermal tests pass. The new regression fails with the former
implementation. A guarded 4K GPU-written UBO oracle passes on M4 with boost
enabled and the requested ceiling verified at 4,104,000/2,892,000 kHz; boost is
restored to its previous disabled setting after the experiment. All 132,710,400
integer values match. No startup, thermal, staleness, progress or watchdog
threshold is relaxed. Full-hardware-frequency requests still avoid pinning a
temporary thermal QoS restriction into a persistent user cap.
