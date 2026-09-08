# Native M4 display bring-up

The G16 renderer and DCP scanout controller are separate devices. The target is
DRM buffer sharing and fenced DCP page flips, replacing simpledrm's CPU copy.
The four-worker simpledrm change is a temporary fallback, not native scanout.

## Imported foundation

The DCP directory is synchronized with Aurora linux-aurora `drm-agx3` at
`4ac2664258f37` for its DCPLink transport, inherited-coprocessor lifecycle,
H17P command implementation and tests. Existing copyrights/licenses remain.
The accompanying RTKit adoption API was adapted separately to retain our
CoreAnalytics work. No T8132 match or firmware bypass has been added here.
The H17P command implementation remains specific to that firmware/platform.

The directory compiled for ARM64. The 35 lifecycle, transport and surface KUnit
tests passed under QEMU arm64. Run from the Linux root with:

```sh
python3 tools/testing/kunit/kunit.py run --arch arm64 --make_options LLVM=1 \
  --build_dir /tmp/dcp-kunit --kunitconfig drivers/gpu/drm/apple/.kunitconfig \
  'apple-dcp*'
```

These are software tests, not M4 display hardware qualification.

## M4 evidence and remaining boundary

Passive RAM captures from J713/T8132 boot
`9cbd6aa5-6d9e-4374-a0f6-38d85b55c0fd` are in the workspace's ignored
`_provenance/omarchy-m4-display/20260908-current/`. The original ADT, bootloader
log, selected firmware segments, addresses and SHA-256 manifests are retained.
Capture used the 60-second watchdog and read-only MTD handles; it did not access
mailboxes or change DART registers. Runtime data is a snapshot, not a coherent
firmware checkpoint. Do not commit the full ADT or firmware blobs.

The DCP text SHA-256 is
`25b095cc293adb48c2c507a578b94810e5e06e7607eb13e5d64a6221e2c29793`.
Disassembly at text offsets `0x1981b8`, `0x198204` through `0x198318` independently
confirms a 0xc0000-byte shared RPC allocation, 0x40000-byte sides, eight total
0x68-byte stream records, four 0x8000-byte buffers per half, and heap offset
0x80000. These agree with the imported DCPLink transport constants. They do not
qualify H17P RPC method IDs, request sizes, display registers or power sequencing
on M4. Those must be checked before a native M4 client issues display commands.

The live ADT records DCP stream 23 and display streams 0 and 16, including
firmware segments and inherited page-table regions. The next integration must
transfer and preserve those mappings, verify the live coprocessor/session state,
and complete the M4 link handshake before buffer submission and flips.

## Firmware dispatch evidence

`tools/m4_dcp_dispatch.py` uses Unicorn to run only the command lookup in the
captured image, stopping before callback invocation. It verifies both input
hashes, skips pointer signing and emits 213 nonzero handlers among A000..A499.
This is offline analysis; it does not execute a display command on hardware.

Inspection of the resulting handlers establishes these decoder bounds:

| Tag | Text-relative handler | Minimum input | Minimum output |
| --- | --- | --- | --- |
| A401 | 0x1e32d0 | No input-size check observed | 4 bytes |
| A406 | 0x1e2f10 | 0x10 bytes | 8 bytes |
| A407 | 0x1e2b80 | 0x1bd8 bytes | 12 bytes |

A407 checks `(input_size >> 3) >= 0x37b` at 0x1e2ba4 and copies 0x1bd8
bytes at 0x1e2c10. The imported H17P request is 0x1d30 bytes: its layout cannot
be assumed correct for this M4 firmware. Field meanings remain to be decoded.
The H17P power method A483 resolves to no handler in this snapshot's lookup.
Neither a matching tag nor a size check establishes full command semantics.

## Handoff boot status

The first passive handoff bundle (m1n1 c19b29c, Linux DT a95bc38570f6) did not
return to SSH. Its validator incorrectly required DCP carveouts to fall inside
the OS-usable boot-args range. The captured boot log gives usable RAM as
0x10002770000..0x103dd1dc000, while firmware occupies reserved physical DRAM
on either side. m1n1 759676a checks ADT physical DRAM bounds instead and adds a
host regression test that rejects the original bug. The corrected release
bundle is built; its installation and hardware boot validation remain pending.
All native DCP/display/DART nodes remain disabled in that DT.

Recovery on 2026-09-08 restored the previously working stage2 bundle, SHA-256
`f037440477898f8e91d8aa5095009e84b5867ce41dab9c4a7d8ffa1cc46928fc`, through
macOS on the verified J713 host. The EFI partition UUID was checked against
`1920ef80-74de-4f1c-a635-80be34cda073` before replacement. Omarchy was blessed
and rebooted successfully: Linux boot ID `0cb07a76-0729-4850-a556-ab2ac806d9c0`,
kernel `7.1.9-dirtyroom-gpu1+` build #9, graphical target active, no failed
systemd units. The kernel and restored bundle hashes matched the pre-experiment
versions. This confirms recovery, not the corrected native handoff or DCP flips.
