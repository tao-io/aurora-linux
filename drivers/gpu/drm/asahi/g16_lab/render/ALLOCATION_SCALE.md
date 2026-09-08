# DRM allocation scale

SuperTuxKart startup reached the old per-file 8,192 GEM object cap with only
431,030,272 bytes charged. DRM mode now uses normal allocation failures and
overflow checks instead of object-count, 64-MiB object-size, 1-GiB GEM-byte,
16,384 mapping-count and 1-GiB mapped-byte cutoffs. Fixture-only memory tests
retain their original budgets. UAT table ownership grows dynamically rather
than stopping at 64 page tables. Address, overlap and PTE validation remain.

`tools/g16allocscale.c`, run through `tools/run-drm-memory.py
--render-allocations`, tests the memory frontend without starting GPU firmware.
On J713, run `20260908T234413Z-drm-memory` passed 8,195 live objects, a lazy
1,088-MiB WRITEBACK allocation, 96 sparse leaf tables, and 1,088 MiB of aliases
to 64 MiB of physical storage. Allocation-size overflow was rejected. Module
unload and node removal passed without a new kernel warning or fault. The
60-second watchdog was disarmed after saving evidence; peak PMP temperature
was 51.5 C. Manifest: `validation/j713-allocation-scale-20260909.json`.

This validates allocation and teardown, not GPU execution at every size. The
earlier async desktop fault remains unresolved; keep async opt-in and disabled
for normal desktop tests. The earlier serial SuperTuxKart warmup reached
76.25 C and the thermal supervisor stopped it; the watchdog then rebooted.
The game launcher now restricts CPU warmup to 150% of one CPU, and thermal
shutdown explicitly stops its service as well as SuperTux. This test quota is
not a driver limit or a substitute for the thermal guard.

An uncapped serial desktop startup also tripped at 76.75 C. Native desktop
experiments therefore request `M4_TEST_CPU_MAX_KHZ=2000000`; the guard selects
supported frequencies no higher than that request or an existing lower cap.
On this J713 that selects 1.8 GHz / 1.86 GHz. The cap persists until reboot,
including on failures, and the guard detects if another service removes it.
The 75 C stop and CPU thermal policy remain enabled. Performance measurements
must report this cap; it is temporary experiment configuration, not a GPU limit.
