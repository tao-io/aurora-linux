# Live J713 D114 qualification, 2026-09-17

Kernel implementation: 0d5dc0bc24db. Installed module SHA256:
920df6487c1ac06fb0bc90897413b08ec132ce829a81f26a585111bd40391e7d.
Boot: 4293580b-1726-4a08-861f-e397d2e95742.

The first real hourly callback arrived at uptime 3611.572 seconds. Four
com.apple.iomfb.pa.piodma_only_regs_count events were accepted and retained
locally. The dictionary was returned intact; the firmware continued operating.
No callback was injected and no firmware timer was accelerated.

Two sequential bounded GPU experiments ran 163 and 168 imported-view pixel
probes respectively, with Vulkan validation enabled and no failures. The
second window covers the real event and requires its count to increase.
Peak temperature: 62.75 C across six PMP channels sampled every 0.5 seconds.
Both windows used the mandatory 60-second watchdog with GPU progress gates.
A pure idle observer had previously caused a watchdog reset because it produced
no GPU retirement. The corrected observer supplies real, checked GPU work;
it does not weaken the guard or count that reset as a driver failure.

After the callback, run-greeter-test.py moved the cursor through 18 positions
on the physical panel. Every move completed a native flip with correct cursor
pixels and no stale trail. The watchdog and thermal guard covered this test;
peak 60 C. Final completed flips: 35; failed: 0. SDDM remains active and all
test processes have stopped. This closes the observed first-hour display
freeze, not an unrestricted multi-hour stability qualification.

Full evidence remains in the workspace:
_provenance/experiments/20260916T235245888762Z-dcp-analytics-pixel-window2/.
The retained subset includes the callback dictionary, kernel event lines,
observation records, experiment report and physical-panel result. Raw scanout
crops and complete kernel/thermal logs remain in the full evidence directory.

live-dictionary-replay.jsonl reconstructs one callback from the actual retained
name and dictionary. Name padding and trailing ABI padding are zero-filled;
it is not a raw callback capture. Append it to the saved startup rpc.jsonl
(with no intervening blank line), then run m4_dcp_native_callbacks_test.py.
ASan/UBSan passes 258 callback inputs and 516 wrong-shape rejections, including
an explicit byte-for-byte check of the returned live dictionary.
