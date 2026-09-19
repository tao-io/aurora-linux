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
