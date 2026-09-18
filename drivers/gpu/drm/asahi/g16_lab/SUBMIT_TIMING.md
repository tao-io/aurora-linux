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
