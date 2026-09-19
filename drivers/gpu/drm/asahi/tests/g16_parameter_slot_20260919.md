# G16 parameter-manager physical slot ownership, 2026-09-19

Overlapping tiling and fragment processing exposed a firmware descriptor alias.
Normal Factorio reached its menu then timed out at job12904, despite five-frame
replays, exact query/pixel checks, and eight fragment-dependency CTS passes.
Evidence: `_provenance/experiments/20260919T134928397320Z-factorio-early-owner-normal-menu`.
Fault dump SHA256 `8c0f99e181ab277beaebe4920ce4f9cea8aa37dd14eee17016fdf97333057f9c`.
Decoded using tools/g16-fault-dump.py, retained build/factorio-early-chained-fault.json.

The stalled second render (index6) requested physical parameter slot1 in its
InitBuffer+8 and RunTA+0x18; manager+0x10 was also1. Manager+0xc was zero.
Only hardware descriptor0 was populated; descriptors1..31 were zero. Descriptor0
contained page-list words 0x12f29400/0x60000080, not this owner's expected
0x11c85c00/0x60000000 (page-list VA0x711c85c000). All managers were encoded with
zero at+0xc, allowing full InitBuffer rebuilds to replace another owner's entry.

25G83 TEXT firmware SHA256:
`197582323a68b5f084212f012ac7bbabffe9e307df3db8ec383b25cf6828640f`.
Source retained under `_provenance/asahi-pivot-20260909/hardware/loaded-firmware-phram-4/gpu-fw-text.bin`.
InitBuffer handler2159c obtains context; staged=0 calls full rebuild2eb4c.
2eb70 and2ec90 load signed manager+12. The latter is scaled by16 at2ecb8 and
added to the descriptor table base at2ecf0. It never assigns the requested
command slot into manager+12. Full rebuild ends with DSB at2ed54.
This is the physical descriptor index; do not confuse it with manager+0x10.

The runtime already reserves disjoint physical slots for in-flight owners
(two banks of16). Populate manager+0xc with that slot. Preserve full firmware
InitBuffer rebuilds and firmware ownership of the live hardware table.
The 32-manager host regression models the witnessed firmware lookup, registers
all owners, then checks each still resolves its own page list. Old encoder
aliases slot0 and fails. All15 dependency/command tests pass with the fix.
ARM64 asahi.ko build passed. Live validation is recorded below when complete;
this initial commit does not assert the opt-in overlap is hardware-qualified.


The descriptor-only commit1c617f9b0b27 FAILED desktop boot validation.
Job41 used manager physical slot16 and descriptor16 held the expected pages,
but both engine register lists still selected slot0. The GPU faulted before
replay startup; experiment140439's process failure is not a new replay fault.
Sealed dump SHA eb74afb40da39984be80b44e51e36f6cbea20bc995a259dce2d292c98c7d3113
is retained in experiments/20260919-factorio-parameter-slot-bootfault. Kernel
c40864f9d5f8 was restored under guard140622 and the desktop recovered.

The companion fix supplies the same owner to TA register0x1c830 and fragment
register0x1c838. This is independently witnessed on M4, not inferred from G17:
- 25G83 ta-registers.c emits descriptor+0x10d0 to0x1c830; fragment-registers.c
  emits its host descriptor+0x830 to0x1c838.
- Native193422 fragment128 capture: RunTA+18=2, TA register1c830=2,
  fragment register1c838=2.
- Native155413 multisample4x capture: the same three selectors are13.
Captures reside under _provenance/asahi-0ad-20260910-native-traces; command
register records begin at TA+2e0 and fragment+434 respectively. Values follow
these 4-byte register numbers as little-endian64-bit words.

Eight native-state host tests pass, including every ID in both runtime banks;
15 command/dependency tests pass. Hardware qualification follows separately.


Kernel79f480d290c9 hardware qualification: boot021be92d, moduleSHA
470bbbbf2e4b0611d558d4ec8ce88f191127d66d5e03626de51986aeb987f623.
Desktop boots normally. Replay142127 and installed-default142944 preserve
all pixels and exact fragment statistics. Live menu142153 passes60seconds;
installed-default143011 passes120seconds, peak88.75C, no GPU fault. All runs
retain60second watchdog and six thermal channels. Separate startup142816
lost its X11 surface then guestSIGSEGV; not a GPU timeout and still unexplained.
Full CTS deferred;5ms GPU-frame target not achieved.
