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

The corrected handoff bundle was subsequently installed with the 60-second
watchdog armed and booted successfully as Linux boot ID
`3c63cca0-5d61-429b-87f6-b389f168660f`. Its SHA-256 is
`6a39858ca10c3c11b5773ef4f065fd82ac219929761db5e4f57b9d13be5dd07e`.
The new stage2 log confirms publication of the firmware mappings. A fresh
watchdog-guarded ADT/FDT capture verified the firmware UUID and all seven
no-map regions with the expected DCP phandle, remap address and rounded size.
Evidence is in `_provenance/omarchy-m4-display/20260908-handoff/` in the workspace.
This qualifies the passive metadata handoff only; native nodes remain disabled.

## Native RTKit and DCPLink startup

The explicit-load `tools/rtkit-probe/m4_dcp_rtkit_main.c` client uses the normal
Linux IOMMU, coherent DMA, mailbox and RTKit APIs. It does not issue H17P display
commands. Its allocations remain pinned until reboot; hot teardown is not yet
qualified. The raw polling diagnostic established protocol 12 and endpoint
bitmaps 0x51f (base 0) and 0x803ffd (base 1). Its early quiescence request was
not acknowledged before system services had been initialized.

On boot `dbe158d2-345f-40a8-bf82-f493fb21577a`, kernel build #10, the native
client completed RTKit startup and DCPLink INIT/READY. Evidence is in the
workspace's `20260908-native-link` display capture directory. The watchdog was
armed for 60 seconds; three thermal samples peaked at 52.25 C, with no thermal
failure. Host allocations used the DCP aperture (for example 0x10fffffc000),
and firmware-supplied buffers were checked against reserved memory and the
native IOMMU mappings before mapping them into the CPU.

The raw READY message was `0x004045bc9ef40101`. The original diagnostic called
the upper field version 64 because that is how the imported H17P helper names
it. M4 disassembly shows that descriptor+12 is instead an alignment: at
0x19843c..0x198458 it takes the maximum of the supplied value and the cache-line
size at global 0x85ce34 (64). Helpers 0x1962b8 and 0x196328 use that same global
to round cache operations. The M4 diagnostic now supplies alignment 64 and
checks the READY alignment and firmware hash 0x45bc9ef4. This interpretation
must be retained when adding nested RPC buffer allocation.

A412 resolves to 0x1e2480 and requires four output bytes, with no input-size
check. Its handler reads a one-byte display flag without a virtual call. The
diagnostic's optional main-display query uses this entry, not H17P method-table
assumptions. On boot `6f5d606d-4adc-4983-b5ab-2ccb652b3175`, the firmware
acknowledged that query with message `0x42` and replaced the output sentinel
with `is_main_display=1`. The guarded run passed with four thermal samples,
peak 53.25 C. Evidence is in `20260908-native-query`. This establishes native
RPC transport; it does not yet qualify display startup, buffer flips or KMS.

On boot `3166b89d-721e-4088-964f-470011792c19`, the root-only diagnostic
bridge completed 100 sequential A412 queries in 1.162 seconds, all returning
01000000. The independent watchdog/thermal guard recorded a 53.75 C peak.
Evidence: `20260908-native-bridge` and `20260908-bridge-query100`.

The first A401 startup test received callback message `0xc00000002` (12 bytes)
but the imported callback buffer selection at 0x40000 contained zeros. The
client refused to acknowledge it and left the watchdog armed for recovery.
Evidence: `20260908-bridge-start`. Callback placement must be qualified before
using the imported callback handlers. The diagnostic now rejects non-D tags
and records the stream headers on a placement mismatch.

The placement capture on boot `69bdd424-0a5f-4d2b-80fe-39084d581236`
found the actual callback at 0x60000: D599, zero input/output bytes. All other
stream headers were empty except the active A401 command at offset zero.
`20260908-bridge-placement` records this evidence. The AP callback buffer must
use the incoming remote bit, rather than always selecting the remote half as
in the imported handler. J713 TEXT 0x1d7760..0x1d7778 independently emits D599
with null pointers and zero sizes. The bounded startup client now acknowledges
only that exact callback shape; other callbacks still stop the test.
