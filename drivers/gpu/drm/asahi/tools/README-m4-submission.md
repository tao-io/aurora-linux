# M4 submission qualification, 2026-09-19

Factorio uses native ARM64 GL and Vulkan through FEX thunks. Main-menu
60 FPS remains the task; this checkpoint qualifies bounded compute batching,
not full parallel engine scheduling or Vulkan conformance.

The G13/G14 WorkQueue/Event model supplies shared firmware/user stamps,
queue event sequences, prepare-before-publication and retained DMA ownership.
M4 private-memory pools add a hardware slot lifetime: distinct in-flight
pools must not all claim slot 1. Allocate slots 1..4 within a bounded batch,
retain the last completed owner of each slot, and return an old owner to its
queue cache only after a replacement of that same slot has retired. Cached
owners only reuse their own slot. The descriptor software identity is its
retained canonical command address.

Each batch publishes its complete ring once and uses firmware stamp barriers
between commands. Render and explicit user timestamp boundaries remain
serialized. Each compute keeps the cache-publication boundary of the old
single-command path. ASID generations now belong to MMU slot bindings rather
than incrementing on every command. In-flight vector capacity is reserved
before publication; publication failures retain every owner.

## Evidence

All hardware runs use the 60-second watchdog, six thermal channels sampled
at 0.5 seconds, 95 C cutoff and normal maximum CPU clocks.

- Baseline `f70c8fd7cb6a`: retained Factorio replay passes; native menu measured
  approximately 16.4 FPS. Queue depth 1.
- A depth-4 window passed 2048 tiny-command readbacks (0.380 ms/command versus
  0.644 ms baseline) but the Factorio replay timed out. Do not call tiny-job
  readback success qualification of realistic queueing.
- Correct ASID generation, atomic publication, dependency barriers, strict
  dependency flags and forced notifier flush alone each left the replay
  timeout. In `20260918T233929138761Z-factorio-batch-flush-frame`, both shared
  stamps reached 0x4000, but the follower at 0x4100 never completed. All pools
  still claimed hardware slot 1.
- `20260918T234315032698Z-factorio-pool-slots-frame`: distinct pool slots and
  retained slot ownership pass the identical replay, with zero differing
  pixels in the 2560x1664 frame-2 output. Peak temperature 72.25 C.
- Six existing host compute-encoding checks and the ARM64 module build pass.

The replay retains its existing offscreen swapchain/memory-rebinding choices.
It is not a claim of unmodified WSI or application stability. Live-menu
measurement follows this checkpoint. Full Vulkan CTS is deferred by request.

TODO: per-client firmware queues and event-slot allocation; mixed
render/compute GPU dependencies; private-pool sharing/growth and teardown;
full priorities/preemption qualification; 60 FPS live Factorio menu; broad CTS.

## Pool reuse and compute-to-render submission

`bc34235d20cb` searches the retired compute cache by hardware slot and VM.
Live evidence `20260918T234801080551Z-factorio-pool-cache-menu` has 12.591 s
initialization, 14.50 FPS tail (14.08–14.85), and 0.450 s compute preparation
in 15.098 s, compared with 4.931 s in 15.069 s before matching cache slots.
The animated menu scene is not frozen; these FPS runs are observations rather
than a controlled shader-throughput ratio.

A compute prefix can now include its terminal render in one scheduler packet.
Render uses private-memory hardware slot 5, separate from compute slots 1..4.
A firmware dependency precedes InitPB/TA; FS retains its ordinary TA dependency
and partial-render support. No following command crosses the render's CPU
query-timestamp-copy boundary. This keeps the existing parameter slot lifetime.

`20260918T235150223978Z-factorio-mixed-batch-frame` passes with zero changed
pixels. `20260918T235216844401Z-factorio-mixed-batch-menu` passes and measures
15.825 FPS (15.45–16.22), peak 65.75 C. Full asynchronous per-client scheduling,
GPU-side timestamp fan-out, and 60 FPS remain TODO.

## Firmware query publication

Fragment-end copies now use repeated native end-timestamp records before
fragment finalization. In saved J713 25G83 firmware TEXT 19364..19524, each
reads the same completed engine timestamps and applies the native nanosecond
conversion to the user pointer pair. Moving finalize also adjusts its relative
restart target, retaining partial-render handling. CPU retirement no longer
copies query results. The existing eight-copy limit and retained destination
mappings remain intact.

`20260919T000753091055Z-factorio-fw-timestamp-query3` passes the 12-query
render/overflow probe: original plus eight destinations are bit-identical;
three overflow timestamps remain ordered; CPU and GPU readback agree.
Six host encoding/bounds checks and the ARM64 module build pass. Broader batches
are not enabled by this change alone.
`20260919T000806308535Z-factorio-fw-timestamp-frame` passes with zero changed
pixels in the retained 2560x1664 frame. Peak temperature 67.25 C.

## Bounded mixed-engine batches

A packet now contains up to four render/compute commands from one GPUVM.
Every command has a strict predecessor dependency; each render keeps its
ordinary TA/fragment dependency for partial rendering. All queue rings are
prepared before doorbells. Timestamp writes now happen before the next engine
can run, including explicit user timestamps. Compute private slots are 1..4,
render private slots 5..8, and render parameter slots 0..3. Retired owners and
cache lookup track the exact hardware slot and VM. Every packet still returns
the scheduler credit before the next bounded packet.

The first desktop batch exposed completion coalescing: both engines executed
and all rings drained, but the first render's private firmware stamp remained
zero while its user stamp reached 0x300. Evidence is the job-6 dump collected
in `20260919T001302637610Z-factorio-full-slots-stage`. A separate parameter-ID
mismatch was also corrected. All three engines now use queue-owned firmware
and user stamps, with wrap-aware completion comparisons, matching WorkQueue.
The intermediate private-stamp builds are not qualified.

`20260919T001654673104Z-factorio-full-shared-frame` passes with zero differing
pixels, peak 72 C. Live menu timing follows. Active-duration sums overlap within
a batch and must not be interpreted as serialized wall time or GPU utilization.
TODO: per-client firmware queues, cross-client engine concurrency and priorities,
persistent shared private pools, broad CTS, and 60 FPS main-menu qualification.

## Eight-command batch measurement

The bound is now eight commands, with disjoint compute/private slots 1..8,
render/private slots 9..16 and parameter slots 0..7. The bound still limits
DMA ownership and scheduler credit occupancy. Batch summaries count each
packet's active interval once; individual command intervals overlap. The
mirrored `gpu-summary-timings.py LOG --tail-seconds 15` reports both, explicitly.

- `20260919T002435318037Z-factorio-depth-eight-frame`: Pass, zero changed pixels.
- `20260919T002501131985Z-factorio-depth-eight-menu`: 20.159 FPS tail
  (19.35–20.89), initialization 11.088 s, peak 67 C. In 14.122 s: 7.911 s
  packet-active, 1.610 s command preparation, 4.905 s GPU timestamp spans.
  The prior four-command menu was 17.718 FPS; scene/pass counts differ, so
  this is not a controlled throughput ratio.
- `20260919T002611221383Z-factorio-depth-eight-queries`: all eight additional
  firmware timestamps remain identical; three overflow timestamps ordered;
  CPU/GPU query results agree.

Normal FIFO/vsync main-menu 60 FPS remains unmet. The next measurement isolates
presentation pacing with a separate diagnostic launcher, not the normal one.

## Prepare one CPU successor

The scheduler admits two packets: one published and one fully prepared.
Preparation may allocate/clear private backing while firmware executes its
predecessor, but publication and hardware-slot replacement remain serialized.
The new explicitly named `submit_serialized_batch` API chains scheduled fences;
the runtime must enforce execution order and propagate the shared earlier error.
The output syncobj still receives the last job's finished fence.

`20260919T004917311684Z-factorio-prepare-ahead-frame`: zero differing pixels.
`20260919T005001354680Z-factorio-prepare-ahead-queries`: Pass, all eight copies
identical and overflow reads ordered. Live `20260919T005024295412Z-factorio-prepare-ahead-menu`
averages 21.497 FPS (20.67--21.95), sampled peak 88.75 C under the requested
80 C passive policy and independent 95 C cutoff. Full CTS remains deferred;
60 FPS and queued consecutive firmware packets remain TODO.

## Keep consecutive same-VM packets queued

The two admitted packets now own disjoint slot banks: compute/private 1..16,
render/private 17..32, render parameter 0..15. Firmware's UMA lookup explicitly
accepts indices through 255 (25G83 TEXT 2af4c..54); the retained table has 16 KiB.
A cancelled preparation releases its bank without rotating into a still-live
bank. Same-VM followers append behind a strict dependency on the previous
packet's final engine stamp; different-VM followers wait for retirement before
changing the shared notification context. All ring consumer indices must leave
space. Busy accounting now measures the union of published intervals.

`20260919T005648832035Z-factorio-queued-packets-frame`: zero differing pixels.
`20260919T005713130464Z-factorio-queued-packets-queries`: Pass.
`20260919T005739706755Z-factorio-queued-packets-menu`: 22.788 FPS tail
(22.14--23.25), with an earlier portion near 28; peak 89.25 C. The final
14.081 s contains 11.557 s published busy intervals and 7.808 s GPU spans.
The animated scene changes; 60 FPS remains unmet. Persistent private pools,
per-client firmware queues and broad CTS remain TODO.

## Persistent render USC allocator

A displaced, fully retired render owner now keeps its UMA descriptor, freelist,
page list and private payload together. The firmware saves allocator cursors at
UMA+20..2c while draining (25G83 TEXT 2b618..670), and reloads those cursors at
2afec..2b008. Host submitted counts advance by two per TA/fragment pass; reuse
requires the matching completed count. Fresh owners still receive zero-backed
storage and initial freelists. Parameter buffers and command state still reset.
No backing is reused across VMs or before its retained-owner retirement rule.

`20260919T010401266925Z-factorio-persistent-render-pool-frame`: zero differing
pixels. `20260919T010635353186Z-factorio-persistent-render-pool-queries`: Pass.
The 60-second untouched menu observation
`20260919T010434836218Z-factorio-persistent-render-pool-menu` passes, tail
22.79 FPS (21.40--23.74), peak 85.5 C. In the final 19.102 s, 5282 render
preparations total 0.231 s (43.8 us mean) and private-payload clearing is zero,
versus 0.338 ms per render before persistence. Dynamic menu changes preclude
an isolated FPS ratio. 60 FPS, persistent compute pools, per-client firmware
queues and broad CTS remain TODO.

## Persistent compute USC allocator (2026-09-19)

Compute now follows the render owner's qualified allocator lifetime: only
same-VM, same-slot owners displaced by a completely retired replacement may be
reused, and UMA completed must equal submitted. Preserve the embedded UMA
record (command+e00..e9f), freelist, page list and private payload as one unit;
advance submitted by one engine instead of resetting to one. Rebuild the
command/preemption/output records. Fresh owners retain their zero initialization.
Firmware drain/reload evidence is the same 25G83 routine cited above.

`20260919T012807757708Z-factorio-persistent-compute-pool-frame`: original
reference matches exactly. `20260919T013219575689Z-factorio-persistent-compute-pool-queries`:
timestamp copies/order pass. The 60-second actual menu profile
`20260919T012837898331Z-factorio-persistent-compute-pool-menu` passes, ending
at 27.03 FPS (25.11--28.92), peak 88.25 C. Over the final 19.158 s,
4626 compute preparations total 0.0475 s (10.26 us mean), versus approximately
230 us before persistent compute state. The scene varies; this is not a paired
FPS ratio. TODO: 60 FPS, per-client firmware queues/preemption, broad CTS.

## Hardware timeline and performance-state evidence

SubmitSummary now samples the actual performance-state register during normal
retirement checks and sums consecutive commands' end-to-start gaps in the shared
24 MHz counter. State counts are polling samples, not durations. Gaps include
firmware/cache work, queue starvation and VM switches; they are not evidence of
one particular cause. The summary tool parses these arrays while remaining
compatible with older logs.

`20260919T015204520709Z-factorio-gpu-timeline-menu`: guarded 60-second unchanged
menu, 29.10 FPS tail (26.41--30.04), peak 91.75 C. Final 19.101 seconds show
all 62,803 state samples at state 9 (1470 MHz), 12.561 seconds of command GPU
spans and 6.536 seconds between commands, with no overlapping spans. Preparation
is 0.352 seconds and published-busy union 19.015 seconds. This changing scene
has more render passes than the previous 35--38 FPS scene. 60 FPS remains unmet.

## Packet-boundary gap classification

The low-volume summary now separates RR/RC/CR/CC engine transitions inside a
fully published packet from transitions across packet boundaries. The latter
include both same-VM handoffs and client switches, so they are not a pure
context-switch cost. Existing GPU timestamps and retirement ordering are unchanged.

`20260919T022534624026Z-factorio-gpu-boundary-menu` completes the guarded
60-second menu, tail 37.53 FPS (33.93--38.17), peak 89 C. Queued transitions
average RR 277 us, RC 263 us, CR 347 us, CC 159 us. Boundary transitions average
RR 1109 us (2164 events), RC 992 us (728), CC 1572 us (718); CR has only five
samples and is not representative. Of the 7.060 s total gaps, 4.266 s cross
packet boundaries. All 64,253 performance-state samples are state 9. This
narrows the next optimization to submission handoffs as well as reducing GPU
commands; it does not establish firmware-only overhead or 60 FPS.
