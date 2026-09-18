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
