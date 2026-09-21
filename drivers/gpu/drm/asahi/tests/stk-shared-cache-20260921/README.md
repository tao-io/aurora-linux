# Shared displaced-owner cache budget, J713, 2026-09-21

**Not enabled.** The default retains the original 128 MiB per-queue/per-engine
limit. This directory records a rejected experiment and its retained patch.

The fixed 128 MiB per-queue/per-engine FIFO cannot retain a typical race's
working set: each render owner includes a 32 MiB USC pool plus parameter storage.
Repeated full-frame submissions displaced and evicted owners before their next
reuse. Zink's prior run allocated 13617 fresh render owners versus 2801 reuses;
this includes loading, so it is not a steady-race hit-rate estimate.

Replace the separately multiplied limits with one 1.5 GiB atomic budget shared by
all queues and both engines. Caches retain their per-queue ownership and FIFO
policy. `put` reserves global bytes before accepting an owner; when full it can
evict only its own safe idle entries, otherwise the new owner bypasses cache.
`take`, eviction, failed insertion and queue destruction return their charge.
The budget applies to idle, fully displaced owners only. It does not bound all
GPU memory, change the in-flight/firmware-last-owner retention rule, or permit
cross-VM or cross-slot reuse. Shared parameter backing is still charged
conservatively once per retaining owner.

This trades a larger hot-client cache for a finite aggregate idle-cache maximum.
Previously N queues could retain N * 256 MiB over their two engine caches.
TODO: pressure-driven shrinker and fair eviction across inactive clients; no
claim is made that the fixed budget is a complete memory-pressure policy.

The production cache test now exercises concurrent queues, aggregate bounds,
complete budget recovery after closing queues, FIFO eviction, take accounting,
oversized entries and allocation failure. It compiles the actual implementation.
Incremental arm64 module build passes.
Module SHA256: `4b28beea578755161311898fc6a5182e1b69a0c59b2209a6340b3b61f20acfec`.
Stage: `20260921T071752313650Z-stk-shared-cache-1536-stage`.
Guarded reboot evidence: `/root/asahi-evidence/stk-shared-cache-1536-reboot-20260921`.

The initial 1 GiB trial (`20260921T071248163464Z-stk-zink-shared-cache-race`)
was insufficient: 9432 fresh versus 1338 reused 4K owners, 12.739 FPS, minimum
4930 MiB available, peak 86.25 C. That trial's replay had exact reference pixels
(`20260921T071202741823Z-stk-zink-shared-cache-replay`). The final 1.5 GiB
candidate is a separate experiment; these earlier results do not qualify it.

Final boot ID: `0ac46102-8e57-41bc-83ad-10b575695a3f`.
All 162 cases from `../stk-batched-map-20260921/cts-cases.txt` passed on this
module: `20260921T072053021735Z-stk-shared-cache-cts`, two workers,
11.159 seconds runner, 14.038 seconds guarded elapsed, peak 82 C. This is a
targeted regression check, not the full Vulkan suite.
Final Zink replay: `20260921T072129965508Z-stk-zink-shared-cache-final-replay`,
exact reference pixels. The offscreen/rebind/pipeline-cache adaptations match
the baseline; no application-support claim rests on adapted replay alone.

The first final-budget live-race attempt
`20260921T072200323375Z-stk-zink-shared-cache-1536-race` stopped before launching
STK because DP-1 was absent. DCPEXT reported repeated link-error notifications
and mode discovery -67 during boot, then acknowledged teardown. The panel and
GPU remained functional. No FPS result belongs to that attempt. Display logs
are retained; recovery uses a guarded reboot of the same module, not reload of
live GPU/display firmware. This boot failure is separate from replay/CTS passes.

Recovery reboot of the same module restored DP-1 and eDP-1 as connected,
boot `ad2d39b4-40f1-486f-91ef-994545a7c94f`. Evidence:
`20260921T072615620717Z-stk-display-recovery-stop` and
`/root/asahi-evidence/stk-display-recovery-reboot-20260921`.

## Disposition: not enabled

The 1.5 GiB trial also failed to demonstrate a useful improvement. Zink measured
12.472 FPS (`20260921T072908985992Z-stk-zink-shared-cache-1536-race-r2`), with
10020 fresh versus 681 reused 4K owners, minimum 4627.6 MiB available. Native
Vulkan measured 17.906 FPS (`20260921T073158499514Z-stk-vulkan-shared-cache-1536-race`).
The larger budget is therefore NOT retained. The default remains 128 MiB per
queue/engine. The trial patch and tests are saved here for diagnosis. A shared
budget without eviction/fairness across caches can retain cold allocations
while an active cache churns; the trial does not yet identify all of that
contention, so simply increasing the constant further is not justified.
