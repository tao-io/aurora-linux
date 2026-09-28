# Host GPU activity telemetry

`/sys/kernel/debug/asahi-m3/activity` and `asahi-m4/activity` expose:

    version=1 generation_ns=N epoch=N pending=0|1 healthy=0|1

This separate endpoint leaves strict completion-v1 readers unchanged. Reads
access host atomics only, without MMIO or the runtime mutex. Epoch parity is
atomic with the pending state: odd means potentially outstanding GPU work,
even means no outstanding published work. The serialized runtime writer marks
pending before publication and clears it only after proven retirement. Repeated
markers do not advance the epoch. Epoch exhaustion stays permanently pending.
Boot starts pending; teardown/failure health is independently latched.

Pending does not count queued CPU-side jobs and idle does not imply user-process
progress, valid output, or whole-system health. A completion need not clear
pending when other published work remains. Consumers must validate generation,
health, monotonic epoch and freshness, and retain their total execution bound.
An idle sample cannot prove that work published after that sample is retired.
This endpoint alone does not authorize changing watchdog policy.

Validation: host tests execute the real shared counter implementation, including
concurrent readers, repeated markers, completion while pending, generation and
saturation. M3 development/publication modules and the M4 module compile.
Hardware qualification and the subsequent guarded policy are recorded below.

## Idle CPU work during bounded experiments

The guard may renew during pipeline compilation only if two healthy activity-v1
snapshots (before and after CPU accounting) have the same even epoch, and CPU
time in the actual experiment process tree has advanced. Pending GPU work,
idle without CPU work, a changed epoch, generation change, fault, malformed or
stale evidence cannot authorize this renewal. The fixed experiment deadline,
60-second watchdog and thermal gates remain mandatory. The regular completion
path is preserved; old kernels without activity telemetry retain its stricter
retirement-only policy.

M3's CPU cap loop formerly reused `limit`, overwriting the total deadline with
1476000. It now uses `cpu_limit`. The regression test exercises the real timeout
selection and cap loop for ordinary, replay and repeated-draw modes with either
CPU-clock policy: all eight combinations reject the original implementation
and pass the correction. This repairs the guard's bound; it does not change
requested test durations or the independent hardware watchdog.

Tests: `test_idle_work_progress.py` rejects CPU-only/pending progress, idle-only
samples, mid-sample transitions, health/generation/epoch/counter failures, PID
reuse, malformed ABI and stale samples. `test_guard_deadline.py` exercises the
M3 total bound. `gpu-activity-probe.py` runs only under an existing guard and
observes transitions around an existing exact shader probe; M3 observed both
states and eight 63x63 mixed-facing renders passed. Cold replay qualification now passes on both devices: M3 100.436 s, M4
149.754 s with shader disk caching disabled. All three ordinary saved frames
match exactly. Both desktops returned. These are validation durations, not FPS.
The first process-group-only monitor missed a new-session child and failed;
that interruption remains retained. The corrected reader follows descendants
across sessions and rejects a replaced launcher identity.

The follow-up publication fast path issues its ordering fence only on an
idle-to-pending transition. Already-pending work changes no observer-visible
state, so additional publication needs no duplicate marker fence. All three
module builds and actual-source concurrent counter tests pass. This follow-up
binary has not yet been deployed; the replay results above used the first
activity module and do not measure a speedup from this fence change.
