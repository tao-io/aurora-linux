# Retire successful packet VM ownership before fence signalling

M4 Packet::finish previously retained its VM job guard for the entire scheduler
packet lifetime. A scheduler/fence reference can outlive verified hardware
retirement and keep deferred GEM closes pending. Use the same success-only
release already applied to M3 in9af263d2f51d. The existing atomic finish latch
still admits only one finisher. Error completion keeps the guard in the packet;
it does not establish retirement or release quarantined DMA resources.

Three tests execute the production finish method with modeled fence/guard
actuators: release before success is visible, failure/late-success quarantine,
and competing successful finishers. The three production VM lifetime tests also
pass (deferred object/file close, outstanding jobs and quarantine).

Module SHA256 da993c65b6633c323b2e6db3cc55085cec11e9f0648e4d7bd6632f9d1444ba13;
loaded build ID caa825e39eddc2901d03b288801d1e44ca23f08b. Normal Hyprland startup
works after reboot. A complete30-frame Satisfactory DX12 menu replay succeeds
on boot2e98652d-9448-4e1c-a8b4-ab32613bc9b6, default packet8, display manager
stopped: check183557613291 completes in46.32seconds; images pass the existing
4/255,1000-pixel menu review policy. Idle Shmem before/after is1,203,952 and
1,231,888KiB. The60second watchdog,0.5second thermals and1GiB memory reserve
are unchanged. Private full capture/evidence lives in gpu-replay-bench.

This fix alone does NOT resolve the separate memory-reserve stop seen when
replaying the large DX12 snapshot alongside the normal desktop. Both packet32
and packet8 desktop-active controls stopped cleanly at that reserve; changing
the descriptor lowering did not remove it. Do not attribute the successful
headless replay solely to this lifetime correction. Fully displaced idle command
caches and firmware-last owners are distinct; this patch does not free the latter.
