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

The current M4 stage2 logs that it skips display reserved-memory setup. The
live ADT records DCP stream 23 and display streams 0 and 16, including firmware
segments and inherited page-table regions. The next integration must transfer
and preserve those mappings, verify the live coprocessor/session state, and
complete the M4 link handshake before bringing up buffer submission and flips.
