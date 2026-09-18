# M4 GPU priorities, 2026-09-18

The frontend passes firmware priority order (0 realtime through 3 low).
G16 now translates that correctly into the shared DRM scheduler, checks
CAP_SYS_NICE for high/realtime queues, and schedules each native command
independently. One hardware credit remains intentional: queued work stays in
the scheduler instead of a firmware FIFO. TA/fragment pairs are indivisible.
Batch allocation completes before publication, predecessor fences preserve
order, and any command error fails unstarted commands in the same submission.

On J713 the identical low/medium contention probe's foreground worst wait fell
from 68.453 ms to 7.053 ms (p95 52.137 to 4.612 ms). Privileged high/normal
contention peaked at 4.983 ms. Normal users were denied high/realtime queues.
A high-priority Hyprland drove the panel at 16.664 ms median / 16.695 ms worst
recorded interval during 4,096 normal-priority compute commands, all independently
read back. Both eDP and Philips DP outputs remain active. 438 atomic cases
returned 326 Pass/112 NS; 402 priority cases returned 114 Pass/288 NS; four
previously repaired failures passed again. No test failures or GPU timeouts.
Every hardware run retained the 60-second watchdog and 95 C thermal guard.

TODO: this does not implement firmware preemption/context switching within a
command. A long-command control still blocked the high-priority foreground
for 41.002 ms. Do not raise COMPUTE_DEPTH to imply preemption: prior firmware
lost-follower/context failures remain unresolved. Also qualify fault injection
and cancellation under staged multi-command submission allocation failure.

Full case identities, source probes, deployment hashes and evidence references
are in Mesa src/asahi/vulkan/tests/m4-desktop/gpu-priority-20260918/README.md in
the sibling checkout. Existing unrelated Mesa native-buoy work was preserved.
