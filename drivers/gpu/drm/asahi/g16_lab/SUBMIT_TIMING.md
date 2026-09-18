# J713 submission timing

Load `asahi` with `debug_flags=8388608` (bit 23, SubmitTiming) for opt-in
host CLOCK_MONOTONIC timestamps at firmware event receipt, each command's
retirement, and before/after packet fence signalling. Unlike KTraceCh this
does not print the firmware's full trace rings. The Rust module parameter
is load-time only; apply changes through the existing guarded reboot flow,
never by hot-reloading a live GPU driver. Leave it disabled for ordinary use.

Use Mesa's `HK_SUBMIT_TIMING=1` with one serial CTS case and its guarded
cts-bulk runner. `tools/submit-timing.py RUN_DIRECTORY` correlates explicit
monotonic fields, not kmsg prefixes (a different clock domain on this port).
An event stamp is captured after waking the completion worker and counting
the message. It does not identify a specific command. Fence logging occurs
after signalling, with both timestamps saved, to avoid delaying the wakeup
with printk. Command timing includes preparation, firmware execution and host
observation; it is not a pure GPU execution measurement.

Evidence 2026-09-16: `20260916T070628346104Z-dx-perf2` passed all 2,500
iterations of the dual-source multi-attachment r16g16b16a16_unorm case with
the 60-second watchdog and thermal guard (peak 63.75 C). Whole submit-to-retire
median was 4.987 ms across five commands; fence signal took 2.75 us and
signal-to-userspace-wait-return 12.0 us. The latest firmware-event-to-command
retirement interval was 47.8 us median. The userspace gap was mostly blend
epilog compilation; equivalent MIN/MAX key canonicalization reduced it
without changing scheduler/firmware behavior. All 11 contended blend timeouts
remain open. See Mesa's `tests/m4-desktop/README-directx-latency.md` for full
reports. No compute-depth or completion-proof changes are part of this work.

2026-09-18: routine G16 command context, completion and retirement diagnostics
now also require SubmitTiming. Extra UMA/status/root reads used only by these
messages are skipped when disabled. Allocation details use Render/Compute.
Fault, stall and timeout evidence remains unconditional. This avoids continuous
journal traffic and formatting while the runtime lock is held; it does not
change command ordering, completion checks, clocks or the queue depth.

Guarded untouched Factorio menu validation with the same private Mesa build:
`20260918T125401742537Z-factorio-quiet-menu-fps`, peak 66.5 C, normal desktop
after reboot, no fault. Gallium invisible HUD measured about 7.5–7.9 FPS, the
same range as before. This is a logging cleanup, not a demonstrated FPS gain.
Per-command `gpu-command-timings.py` analysis now requires SubmitTiming at load;
use application FPS logging for ordinary measurements without that overhead.

`debug_flags=16777216` (bit 24, SubmitSummary) retains exact-retirement evidence
without per-command diagnostics. At most once per second, after signalling a
completed packet, it emits the recognizable `userspace GPU job ... retired`
record with an explicit CLOCK_MONOTONIC timestamp and the interval's render and
compute command counts, preparation times, active times and GPU timestamp spans.
These are sums across all clients. Active time includes observation latency;
render GPU spans include the TA-to-fragment interval. Neither is a utilization
counter. The first interval also includes idle time since runtime initialization.

The existing experiment watchdog accepts only these fresh, increasing exact
retirements; summaries do not introduce idle keepalives or relax thermal gates.
This flag remains opt-in. Guarded qualification:
`20260918T154850784274Z-factorio-summary-menu`, peak 71.75 C, completed the
long menu observation with fresh exact-retirement watchdog progress and no
GPU fault. Its final 19.093 seconds contained 2537 render and 2531 compute
commands. Render preparation totaled 2.277 seconds, active spans 9.369 seconds
and GPU spans 6.464 seconds; compute totals were 0.282, 3.404 and 0.857 seconds.
The main-menu sample averaged 9.485 FPS; it is not a fixed-scene benchmark.

Render summaries additionally count private-pool bytes cleared and the time
spent clearing that pool and all render buffers. Timing reads are enabled only
with SubmitSummary. Fresh owners report zero recycled-buffer clears.

Render freelist-image reuse (`a6e2e4ec4094`) passed the retained Factorio replay
with zero differing pixels (`20260918T160129850938Z-factorio-freelist-frame`)
and all timer/statistics reset, rollover and ordering checks
(`20260918T160648949608Z-factorio-freelist-query-check`).
`20260918T160154990032Z-factorio-freelist-menu` measured 11.203 FPS
(11.000–11.370), peak 65 C, without menu interaction. Dynamic scene variation
prevents attributing the whole FPS difference to this change. Final 19.069 s:
2568 render commands, preparation 1.948 s, all clears 1.875 s, private clears
1.796 s / 36,035,657,728 bytes (14,032,577 bytes per render on average).
Private-pool clearing dominates preparation; other clears are only 0.079 s.
No reset requirements, completion checks or GPU clocks were weakened.

SubmitSummary also separates the existing hardware timestamps into tiling,
TA-end to fragment-start, and fragment spans. The interstage interval is signed
so overlapping stages cannot wrap into a huge idle-time claim. All three use
the same firmware 24 MHz clock; no AP-clock correlation is needed. These remain
aggregates of command timestamps, not sampled engine utilization counters.

Guarded stage separation: `20260918T163145148861Z-factorio-stage-timing-menu`
retained 19.063 s with 2715 renders and 3533 computes. Render GPU spans totaled
6.860 s: tiling 0.056859 s, interstage 0.229157 s, fragment 6.574297 s.
About 96% of that span was fragment execution; mean interstage gap was 84 us.
This directed the following optimizations toward fragment executables rather
than removing synchronization between tiling and fragments.

The summary also retains up to 16 ASID/render-size buckets per interval, with
count, preparation, active and fragment time plus maximum fragment duration.
Additional geometries increment an explicit overflow counter rather than being
silently assigned to another bucket. Completion messages, snapshots, late
retirements and fallback-wait timeouts are cumulative counters; take deltas
across intervals. Collection remains behind SubmitSummary and reports after
proven packet completion, without changing admission, waits or retirement.

Guarded geometry qualification: `20260918T172024748821Z-factorio-geometry-menu`
passed, peak 65 C. Last 19.054 s: 2926 renders, 3165 computes; 9077
completion messages, 42320 snapshots and only 9 fallback wait timeouts.
No geometry overflow. Factorio ASID 8 issued 2187 full-size (2512x1552)
and 243 half-size renders. Their fragment spans totaled 4.5094 s and
0.04835 s; full-size preparation took 1.1677 s and active spans 6.9771 s.
The compositor's 490 panel renders added 1.1766 s fragment time. This
points to per-pass fragment cost and command turnaround, not frequent
missing interrupts; it does not establish 60 FPS or sampled utilization.
