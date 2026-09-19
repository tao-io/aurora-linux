# G16G compute timestamp fanout (25G83 firmware)

Feature bit5 extends compute payloads from64 to128 bytes with eight timestamp
handles/offsets. Older64-byte payloads zero-extend. Extra destinations use the
same bounds/alignment/type validation and retained mapping lifetime as the
primary timestamp. Nonzero copies require a primary end destination. G17 does
not advertise this feature and rejects nonzero copies.

The original compute SKU/state at+0x900 stays intact. An extended executable
sequence lives at+0x1000 and pairs at+0x1800 in an8KB command allocation (still
one16KB physical page). Repeat the existing end Timestamp record, relocate
FinalizeCompute, and fix its relative restart. No new hardware opcode, no CPU
clock substitution, no removal of engine completion or cache boundaries.

Host encoding tests7/7 pass, including0..8 destinations and invalid bounds.
Production byte parser tests pass old/new payloads and unknown-tail rejection.
Incremental J713 module build passes. Installed module SHA256:
13f5e54c33250d7db3a67d8be3baeb9b5efccebbc2749a39bd00a42bab3f5695.

20260919T064940021913Z guarded J713 probe passes all32 combinations of counts
1/9/10/64 and result flags0..7. Checks exact equality within each nine-query
fanout group, monotonicity, CPU/GPU agreement, padding and fill preservation.
Watchdog60s, six sensors0.5s,95C cutoff; peak61.25C. An existing Mesa optimization
which moved a timestamp onto its own query-copy consumer needed a visibility
fix first (064731302696Z failure,064825867342Z diagnosis). The feature remains
opt-in in Mesa. This is focused qualification, not full CTS or60FPS achievement.

Original complete Factorio frame064957928442Z passes with zero differing
pixels from retained reference88594c10407e243eb0617860b8f2abc1f06c0844ec67ddba3900875d7e36796e.
Replay uses the recorded offscreen/rebind adaptation; this is not a live FPS claim.
