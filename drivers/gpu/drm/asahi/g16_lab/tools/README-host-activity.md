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
Hardware validation is pending; no experiment guard uses this endpoint yet.
