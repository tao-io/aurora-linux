# Fully displaced compute-owner reuse, J713 / 25G83

Mixed engine packets need not allocate a new compute owner when a fully
retired, displaced owner in the same GPUVM is available at another slot.
Keep the existing exact-slot preference; on a miss, retarget an idle owner.
Preserve its complete USC allocator, page list, private payload, ASID lease,
command mappings and cumulative submit/completion counts. Restore the saved
UMA descriptor before rewriting its slot and canonical owner address.

Only the existing exact-slot replacement retirement path admits an owner to
the cache. This change does not reuse an in-flight or firmware-last owner,
reuse backing across GPUVMs, reset allocator cursors, or skip completion checks.

Two packet banks can retire 32 distinct 32 MiB compute pools. The old 768 MiB
per-cache bound cannot retain that working set. Allow 1280 MiB per idle cache
and 2048 MiB device-wide, with the existing lazy allocation, queue LRU,
oversized bypass, allocation-failure handling and close/Drop reclamation.
These are caps, not reservations. Default packet size remains unchanged.

## Evidence

Module SHA256 `af4b29da29ba3cc1262617f7eb2ff25d429e4cef3920cdbde98ae301342eff3b`;
boot `4f29ed70-6fb5-41aa-825a-db7198c630ce`. Mesa isolated control SHA256
`a2f968f93fafcaa8d42c1e921ba4ce30232c21cd9c68ed21955ce085792e9694`, native
read-only preambles explicitly enabled. Normal desktop Mesa is unchanged.

- 15 host compute/timestamp tests pass, including a descriptor rebind test
  over all 32 slots with nonzero allocator state and untouched outer bytes.
- Production cache host tests pass: aggregate/per-cache bounds, queue LRU,
  self eviction, close/Drop release, failed allocation and oversize bypass.
- Exact Satisfactory RGB frames 1/16/31 pass (`20260926T071517660780Z`).
  Same 3840x2160 capture and adaptations; no game launch or reduced settings.
- Matched CPU ceilings 1800/1860 MHz, packet size 32: original cache median
  1.89736 FPS; global-only 1536 MiB 2.47569 FPS; final reuse/budget 3.65155 FPS
  (3.65155, 3.65234, 3.64201). Original packet-8 baseline 3.63253 FPS.
  Thus this fixes the larger-packet regression; it is not a large improvement
  over ordinary packet-8 operation, nor attainment of the 5 FPS target.
- Timed replay cache misses/evictions stop growing; compute preparation falls
  from roughly 200-250 ms to about 5 ms per one-second summary interval.
- 10 pinned Vulkan compute/synchronization cases pass
  (`20260926T072249911939Z`); frozen indices and QPA evidence retained.
- All 13 reset/copy/timestamp visibility modes pass
  (`20260926T072255115793Z`), including optional compute stamp coalescing.
- Normal Hyprland/Foot observation passes (`20260926T072316660927Z`); retained
  panel capture visually reviewed. This is a short smoke test, not long-term
  stability qualification.

All hardware segments held the mandatory 60-second watchdog and 0.5-second
thermal guard. Full assets, source/build hashes, measurements, exact backups
and rejected intermediate trials remain in the private local benchmark repo.
