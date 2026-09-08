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

With the corrected buffer selection, D599 was acknowledged and A401 advanced
to D108 (no input, four output bytes), on boot
`1e3eed1f-043e-4168-ab60-826d484806a1`. The client stopped at the unknown
callback and preserved `20260908-bridge-start-fixed` before watchdog recovery.
Offline M4 call sites qualify D108/D109/D110/D111 as PROV/PROD/PMUS/IOMF service
creation, respectively (0x265e20, 0x2700b0, 0x260c74, 0x26d708). Each reads a
boolean from byte zero of a four-byte reply. These IDs differ from H17P.
The startup client answers those exact shapes and uses the M4 A390 flag query
(0x272d3c, no input/four output bytes) for the D100 nested handshake.

On boot `c5a676ff-941d-4a28-bdeb-37dc5178fd9a`, A401 passed D599 and all four
service-creation handlers, including a repeated D108. The next callback was
D406, a 72-byte IOMF `InitialPanelTemperature` property publication with no
output. The client records and acknowledges this exact property shape, matching
`iomfb_set_fx_prop_req`; no thermal policy or clock change is requested.
Evidence: `20260908-services-start`. Full startup is still unqualified.

`tools/m4_dcp_swap_layout.py` executes only the captured A407 marshalling wrapper
in Unicorn, substituting allocation, surface constructors and the final display
virtual call. It checks both all-null and all-present surface lists. The checks
pass for the hash-pinned M4 image: wire size 0x1bd8, four primary surfaces at
0x588 with stride 0x22c, primary IOVAs at 0xe38, extra words at 0xe58, six secondary
surfaces at 0xe78, and secondary IOVAs at 0x1b80. It never runs the actual display
implementation and does not establish a successful hardware flip or the internal
meaning of the swap-header fields.

On boot `eba2d4d3-7e6c-43e1-b8b7-0ddab394365c`, inherited-state A406 returned
swap ID 4 and status 0 without A401 startup. Evidence:
`20260908-inherited-swap-start`, peak 53.25 C. A following all-null, no-change
A407 request using the older +0x50 swap-ID field was rejected with translated
status 1 and `IOMFB_SWAP_SUBMIT_LOST`, logging transaction ID 0. It did not
complete a flip; `20260908-noop-swap4` records the failure, peak 53.75 C.

The M4 implementation at 0x1d5074 saves input x1 in x20 (0x1d50b4) and reads
the swap ID at +0x98 (0x1d50c8). Its failure log at 0x1d6350 also reads +0x98,
with enabled/completed masks at +0x14c/+0x150. The guarded `--noop-swap` test now
obtains a fresh ID from A406 and uses these M4 fields. It requires a zero A407
status at response+5 and a matching D590 completion; an RPC reply alone is
insufficient. M4 D590 is emitted at 0x1dced0 with 0x730 input bytes and no output.

The corrected +0x98 no-op request on boot
`c478036e-3718-4752-813b-a550f1cb7458` received a zero A407 status, with no
lost-transaction log. No completion arrived within five seconds, so the test
failed its completion requirement and left the watchdog armed. Evidence:
`20260908-noop-fixed`, peak 53.75 C. An empty enabled/completed mask may not
schedule a display update; the next bounded test can update only the background
using bit 31 in both masks and the color at +0x154. This retains all surface
slots and distinguishes accepted RPC submission from display completion.

The background-only request on boot `ce8c2358-e163-4221-bbcb-c75618ef4b5a`
also returned A407 status 0 without a D590 completion within five seconds.
Evidence: `20260908-background`, peak 54.25 C. Therefore accepted inherited-state
swaps alone do not qualify the completion path. Display startup/client state
must be investigated before assuming that native KMS can retire framebuffers.

A457 (no input/output, virtual slot 0x1c0, inferred first-client notification)
was acknowledged on boot `102786ac-a1b7-4091-bfae-54088997c486`; repeating the
background swap still lacked completion. Evidence: `20260908-client-open` and
`20260908-client-background`. Further inherited-state variations are deferred
in favor of completing startup.

The first nested A412 test during A401/D599 on boot
`bf6ed78a-7a89-4f7a-8337-42a5fb1b5458` sent message 0x1000400202, reusing
the parent stream at offset 64. Firmware asserted `link_msg_int_handler
stream:0 not free`. This disproved the same-stream nesting hypothesis. Evidence:
`20260908-nested-start`, including the RTKit crash mailbox history.

The opposite-half attempt on boot `d3e808be-070a-43f0-b9f0-801607831efa`
also asserted, with `link_msg_int_handler failed with 0x1000000302`.
Evidence: `20260908-nested-opposite-start`. Neither initial hypothesis qualified
nested execution.

`tools/m4_dcp_nested_route.py` reproduces both assertions by executing the
hash-pinned IRQ handler at 0x197e74 offline with an occupied parent stream.
Bit 9 selects a new-stream interrupt path: it requires owner != firmware side
and a free remote stream. It is not a recursive-call marker. With bit 9 clear,
message 0x1000400002 takes the normal handler. The callback wait loop at
0x195f78 dispatches non-reply messages through 0x196444 before resuming its wait.
The diagnostic therefore re-enters the callback's stream, preserves the parent
packet at offset zero, uses offset 64 for the nested query, and keeps bit 9 clear.
This route is qualified by the hardware test below. The bridge immediately wakes
its reader on an RTKit crash. Arbitrary deeper command nesting is unsupported.

The startup client also implements host property storage with exact M4 wrapper
sizes: D102/D104 (68/0), D107 (64/0), D401 (80/12), D406 (72/0), D414 (80/4),
and D415 (76/4). Call sites 0x2722e4, 0x272424, 0x27254c, 0x2397f0,
0x23a004, 0x23a2e4 and 0x23a388 establish those packet shapes. Missing integer
properties return false; publications are logged or stored, without clock or
power changes.

On boot `00dc3ee8-f430-4895-9d80-4c032f484167`, the D599 callback successfully
re-entered A412 and received 01000000 with reply 0x42. A401 then continued through
D108/D109/D110/D111, D406 and three D414 publications to D413 carrying
`M3TimingParameters`. Evidence: `20260908-reentry-start`, peak 52.5 C. The test
stopped at D413; this qualifies recursive transport, not completed startup.

Dictionary publishers D413 (4168/4), D552/D561 (4164/4), and scalar/string
properties D563 (76/4), D565 (72/4), D567 (128/4) now have exact-size host handlers.
The M4 sites are 0x23a224, 0x1db450, 0x1dbed4, 0x1dc180, 0x1dc394, 0x1dc5a8.
Dictionary bytes are retained as host metadata, without interpreting padding as
part of the serialized object or changing display hardware state.

On boot `b47a936a-3ff5-4b31-b70a-2a40ef4adc24`, startup passed the recursive
query and published timing dictionaries and display metadata (2560x1664, timing
mode 2). It stopped at D112, peak 53 C (`20260908-properties-start`). The M4
startup call sites 0x26e4dc/0x26e57c issue D112/D113 with no input and four-byte
boolean output, extending the host service creation handlers.

D112/D113 passed on boot `fda4641c-ca7f-407c-a032-dc33c784c95e`; startup next
emitted D575 (88/76), the hotplug notification. M4 wrapper 0x1dcae8 establishes
the connection u64, 76-byte in/out field and null byte at +84; the host preserves
that field. D576 at 0x1dcbf0 publishes two power-state bytes (4/0). Evidence:
`20260908-services6-start`, peak 52.5 C.

The hotplug notification reported connected=1 on boot
`55123d7e-2ff7-4318-8739-9829c04478de`, then startup stopped at D300 (16/0).
The publisher at 0x1ca55c passes two u32 fields and a u64 value. The client now
logs it. Offline inspection also qualifies the M4 chunked metadata sequence
D127 (4/4), D128 (0x1008/4), D129 (64/4) at 0x257c78..0x257ddc. The receiver
bounds total length to 1 MiB, requires contiguous chunks and saves completed
serialized properties. Evidence: `20260908-hotplug-start`, peak 54.25 C.

Startup reached the PMU service match D206 on boot
`098348eb-842e-4c02-aefa-80c6dc623ff9`, after D100 successfully re-entered A390.
The M4 D206/D207 callbacks at 0x2511f4/0x251250 use recursive A131/A132, whose
wrappers at 0x2514e0/0x251394 return a translated status in four bytes. The host
requires status zero before returning service-match success. Evidence:
`20260908-metadata-start`, peak 53.75 C.

Both PMU/backlight matches completed with status zero on boot
`807626fb-37c6-4815-bca0-bd34b64295d2`. Startup next requested D408 for PROV
clock index 0. Its M4 wrapper is 0x2394a4; the built-in host-less implementation
at 0x2394f8 returns 48 MHz for index 0 and 24 MHz for index 1. The host handler
uses exactly these two rates, rejects other services/indices, and does not
program clock hardware. Neo's 0x154/0x194 clock IDs do not apply here.
Evidence: `20260908-pmu-start`, peak 53.75 C.

The reference clock reply passed on boot `cb6f72a7-4587-4aad-af75-1888f26828e2`.
D125 then requested the optional `initial-vbi-advance-lines` EDT override. Its
M4 wrapper (0x272a34..0x272abc, 100/36) sends a key, element count and up to eight
u32 defaults, then unconditionally copies the returned values before reading
the success flag. The host preserves only the declared defaults and returns
false; zeroing the whole response would incorrectly erase them. Evidence:
`20260908-clocks-start`, peak 61.25 C.
