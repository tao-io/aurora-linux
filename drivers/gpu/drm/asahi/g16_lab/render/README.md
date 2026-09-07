# J713 vertex/fragment bring-up

Status: Linux buffer and packet preparation implemented and exercised on J713.
No Linux vertex or fragment execution has been established. Existing Mandelbrot
compute validation remains separate.

`stage=render-prepare` imports the 34 encoded capture mappings, allocates a
4 MiB parameter pool, generates its 128 virtual 32 KiB page IDs, initializes
the hardware parameter-manager state and builds TA/fragment job envelopes and
SKU streams. It now exports a 512 KiB preparation snapshot as
`/sys/kernel/debug/g16_lab_render_prepared`. Module removal releases all of
these allocations. This stage does not publish UAT mappings or submit work.
It builds private 42-bit UAT trees and verifies every leaf against its allocated
Linux page. The earlier 64 KiB graph had 475 user leaves in ten table pages
and four firmware leaves in three table pages on J713. The expanded graph and
new root-3 address have compiled but have not yet been run on hardware. The firmware tree contains only the
new command graph; it still needs the shared firmware mappings before use.
Captured-resource permissions currently use the working compute AP2 executable
tuple provisionally, rather than claiming captured render PTE evidence.
The register blocks currently contain only their stage's initial register;
the remaining draw register inputs and shader submission are pending.

`g16_render_ta.h` now translates the native TA register producer, including its
65-write baseline, conditional registers, pointer masks, replicated register
values and mode word. It takes an offset-labelled descriptor plus explicit
platform/PB inputs and never dereferences native CPU pointers. Host tests cover
its alias writes, masks and mode encoding; the header also compiles in the
kernel build. This generator is not yet called by `render-prepare`: the
descriptor's render geometry, private buffer addresses and platform defaults
still need to be supplied from a reconstructed draw. The fragment producer in `g16_render_fragment.h` is also translated. Both
still need draw inputs before submission.

Prepare the firmware input with:

```sh
python3 tools/pack-render-resources.py render/reference /tmp/render-reference.bin
sudo install -D -m 0644 /tmp/render-reference.bin /lib/firmware/g16/render-reference.bin
sudo python3 tools/experiment_watchdog.py -- insmod ./g16_lab.ko stage=render-prepare
sudo cp /sys/kernel/debug/g16_lab_render_prepared /tmp/render-prepared.bin
sudo rmmod g16_lab
```

The packer verifies the stored resource hashes and includes only encoded-stage
payloads. It excludes `expected-tile.rgba`. The kernel validates sizes, GPU VA
ranges, directory offsets and overlap before copying the payloads.

The initial J713 preparation run returned stage_result=0 and unloaded normally.
The exported snapshot was independently decoded to check both command types,
context generations, register pointers, SKU start/finish/restart/end records,
manager counters and all 128 page IDs. Snapshot SHA-256:
`cd11f7eaa6c01487ca31a1f1950bd61ea9be93a81b9deaee082f4365924aae8a`.
This is CPU preparation evidence, not a GPU execution result. That snapshot
predates the buffer-info pointer correction, expanded graph and root-3 rebase.

Host tests exercise register-capacity boundaries, rejection without partial
writes, 25-bit page-ID limits, unaligned encoding and SKU restart targets:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    render/test_pack.c -o /tmp/g16-render-test
/tmp/g16-render-test
```

The source is the checksummed `009-command-render-draw` case in the local
M4/25G83 corpus. `tools/prepare-render-reference.py` verifies every consumed
source against the original SHA256SUMS, checks GPU mappings for alignment,
bounds and overlapping contents, and packages the 34 unique mappings using
deterministic gzip. Original files are not modified. This selected copy is
not a backup of the complete corpus.

The probe's `command_vertex` loads a float2 position by vertex_id from its
geometry buffer and emits (x,y,0,1). `command_fragment` emits
(0.25,0.5,0.75,1). The captured 4x4 texture changes only at bytes 0x100..0x13f,
which become sixteen little-endian RGBA8 words 0xffbf8040. `expected-tile.rgba`
is the native reference output, never a Linux GPU result or an upload input.

Shader records are identified for this commandprobe's allocation order:
record 0 is compute, record 1 fragment, record 2 vertex. This differs from the
shader-microprogram probe, which places fragment in record 0 and vertex in
record 1. Treating the archive index as an architectural stage is incorrect.
Both render shaders match the separately saved gated render-draw capture
byte-for-byte, despite different arena offsets caused by its longer compute
shader. The fragment is 52 bytes, vertex 86 bytes; hashes and offsets are in
reference/reference.json.

Register/firmware source references (macOS 25G83 AGXG16G):

- TA register builder: 0xfffffe00089cbcb0. Command register list begins at
  +0x40, its GPU address is at +0x740, count/length at +0x748/+0x74a.
- Fragment register builder: 0xfffffe00089c162c. Register list begins at +0x80,
  GPU address at +0x780, count/length at +0x788/+0x78a.
- Entries retain the packed 12-byte register/flag/value grammar shared by our
  working compute path. List values and firmware tails are stage-specific.
- TA submit: 0xfffffe0008a02a7c, command type 0, JobMeta starts at +0x878.
- Fragment submit: 0xfffffe000894aa94, command type 1.
- TA SKU encoder: 0xfffffe00089cb624. Start opcode 5 copies 0x1c8 operand
  bytes; opcode 1 is TA-finish WFI; finish opcode 6 has a 0x94-byte record.
- Fragment SKU encoder: 0xfffffe00089bf3f8.
- Both TA and fragment WFI records contain the word 1. Fragment's producer
  is `AGX3DChannelG16::encodePixelEndRenderWFI`, 0xfffffe00089bedd0.
- Parameter manager reset: 0xfffffe00089e0ee4; page-list construction:
  0xfffffe00089e0bd8; virtual backing-page ID encoding: 0xfffffe00089e4010.
  The 0xc0-byte hardware manager and 0x80-byte per-pass PB slot are different
  allocations. SKU start operations point to the latter.

These addresses refer to the original local disassembly/decompilation under
`drivers_m4/work/gpu-kext-decompile` and `/tmp/g16g-kext.dis`. They are evidence
locations, not callable Linux addresses. Neo G17P uses a different SKSM/dual-ASC
protocol and must not supply these G16 command offsets.

The captured resources include shader/binding/control-stream data and the
readback blit. They do not yet provide a validated live TA/fragment firmware
command or a complete kernel-owned parameter-buffer/queue/SKU graph. The next
integration must reconstruct those structures, populate Linux-owned backing,
and verify real output plus stage completion; queue counters alone are not
shader-execution proof.

## Producer/firmware cross-check

The local J713 firmware decompilation's `FUN_00018030` consumes the same
register-list pointers and completion fields emitted by the native G16 host
encoders. Offsets in this table include the four-byte SKU opcode:

| Field | TA | Fragment |
| --- | --- | --- |
| Start opcode / record bytes | 5 / 0x1cc | 7 / 0x1ec |
| Start register-block pointer | +0x14 | +0x14 |
| Start parameter-buffer-state pointer | +0x24 | +0x1c |
| Start slot-array pointer | +0x2c | +0x24 |
| Start context ID | +0x44 | +0x5c |
| Start attachment array / count | +0x88 / +0x188 | +0xa8 / +0x1a8 |
| Start UMA binding | +0x190 | +0x1b0 |
| Finish opcode / record bytes | 6 / 0x94 | 8 / 0xbc |
| Finish firmware stamp pointer / value | +0x48 / +0x50 | +0x0c / +0x14 |
| Finish signed restart displacement | +0x80 | +0xa0 |
| Finish context-store pointer (unaligned) | +0x85 | +0xa5 |

Both start operations dereference the register block's +0x700 GPU address,
+0x708 host count and +0x600 attachment aperture storage. Firmware appends
stage-specific register writes after the host list. Host entries therefore
must leave room before +0x600, rather than treating everything up to +0x700
as register capacity. TA appends three context-selector writes at this point;
fragment appends two. These source observations are not live execution tests.

The start operations dereference parameter-buffer state and per-slot state
before dispatching hardware. Uploading the captured user resources alone
does not populate that kernel-owned graph. The finish operations also use
stage-specific state, including backward restart displacements; substituting
a compute or Neo finish record is not a valid reconstruction.

The Ocean report explicitly records `stage=compute program=ocean`. Its verified
image is compute evidence. The separate commandprobe draw is the vertex and
fragment reference used here.

## Native producer differential validation

`tools/test-render-native.py` compares the portable C code with the original
M4/25G83 ARM64 producer routines under Unicorn 2.1.4. It requires a separately
owned native kext with SHA-256
`5a62d601d5b32301efcd89b144c08874c601d80b40f4b2fd64819537e07a31cf`.
No native executable is included. The tool adapts pointer-auth instructions
only in its private emulator memory, supplies explicit platform defaults,
and disables global debug overrides. It executes the native register encoder
and geometry arithmetic, including conditional register paths.

The recorded run passes 100 deterministic cases each for TA register lists,
fragment register lists, single-sample/one-layer geometry, and hardware PB
packing. Register bytes, counts and stage mode words match exactly. Geometry
covers both 16/32 utile dimensions and image sizes up to 16384. The PB test
also verifies preservation of native reserved bits. These are offline
translation checks, not live vertex/fragment execution.

`g16_render_geometry.h` implements that restricted geometry path and the
16-byte hardware PB descriptor, which is separate from both the firmware
manager and per-pass PB slot. The preparation graph's buffer-info pointer now
correctly selects its actual manager at +0x9000. The older snapshot hash above
predates this pointer correction.

The VDM control stream is descriptor +0xea8 / register 0x1c880. Descriptor
+0xf48/+0xf50 and registers 0x10110/0x10118 instead bind preemption buffers.
The synthetic TA register unit test exercises pointer propagation, not the
semantic identity of those test addresses.

## Independent live queue gate (J713 validated)

`render_queue_probe=1` with the complete validated Mandelbrot module parameters
prepares the new graph, attaches TA/fragment runtime pipe descriptors before
firmware boot, and runs the existing Mandelbrot check first. Only after that
passes does it publish context 6 under the UAT handoff lock and submit an
already-satisfied barrier on each render queue. A pass requires all queue
cursors to retire and the queue to remain idle for 50 ms without observed
firmware recovery. This gate does **not** submit vertex or fragment shaders.

The graph now occupies `fffffc3000000000..fffffc300007ffff`. Its root-3 branch
is checked empty in the resident firmware and kernel lower address spaces.
Each queue-info allocation has room for the native 0x24c0-byte object;
queue-info objects no longer overlap at 0x400-byte intervals. Queue and pipe
rings, scheduler contexts and notifier lists have separate allocations within
the graph. The context-6 upper root borrows the existing firmware/UMA mappings
and adds the graph branch; its lower root borrows only the UMA branch at index
7. Borrowed table pages remain owned by the original initdata allocation.

The bootstrap stops ASC before detaching these mappings. Detachment clears the
new TTBAT slot, removes the inserted root entries, restores the runtime pipe
descriptors and invalidates the relevant contexts before allocation release.
The graph remains available through debugfs until module removal. A nonzero
`stage_result` is a failure even when module insertion retains diagnostics and
returns success.

Validation so far: arm64 module build and existing host packet tests with
ASan/UBSan. Live validation is pending: on 2026-09-07 the M4 was offline on
Tailscale and unreachable at its previous LAN address. Use a fresh boot before
starting the firmware session, as with the successful compute runs.

The existing evidence runner supports this gate with
`tools/run-mandelbrot.py --render-queues --module /root/g16_lab.ko` on J713.
It checks the render bundle checksum, preserves the expanded graph, and requires
both queue retirement messages plus mapping removal in addition to the exact
Mandelbrot image. Its manifest explicitly records that no render shaders were
submitted. It retains the fresh-boot guard and leaves a failed module loaded
for diagnostic inspection.

## First-use parameter buffer packet

Preparation now emits the native 32-byte type-6 InitPB packet at graph +0x8800
and a 16-byte hardware PB descriptor at +0x20000. The native producer
`AGXTAChannelSKU::submitBuffer` (8a02a7c) puts context, hardware PB ID, staged
block cursors, firmware manager pointer and the following TA completion value
in that packet. Firmware 1cf74 attaches it to the next scheduler node; 21434
consumes it before launching that node's TA job. It must precede TA on the
same queue and cannot serve as a standalone PB completion test.

The initial hardware ring is zero, as in `buildPageList` (89e0bd8), with the
manager's 128-page backing count. The page-list GPU pointer now retains bit 42
when packed into the hardware descriptor, selecting the upper root instead of
applying the lower-address 0x7000000000 bias. The manager retains the canonical
pointer; the hardware encoder consumes its low 43 bits. Tests cover this
first-use encoding and the unaligned InitPB pointer field. Neither packet is
published to hardware by the barrier-only queue gate.

Hardware validation on 2026-09-07, boot
`6fb70b2f-989d-40ba-8e4f-c05de1488a1d`: the independent TA and fragment queues
both retired their barriers and remained idle for 50 ms, context 6 was
published and detached, exact Mandelbrot verification passed, and module
removal returned zero. No shader was submitted on either render queue.
The first attachment checks stopped before firmware boot because unused
runtime pipes contain eight copies of the zero-scratch-page VA, not eight
NULL pointers. Attachment now validates that exact scratch descriptor before
replacing it and preserves it for restoration.

## Experimental tiling submission

`--render-ta` on the evidence runner enables `render_ta_probe=1` in addition to
the queue gate. It publishes the PB hardware table at the cold bootstrap,
retains the exact compute check, then sends InitPB followed by a TA job. The
job uses the captured resource-29 VDM stream, a 4x4 one-layer/one-sample tile
geometry, the M4 Metal +b8/+c0 primitive constants, and the live shared USC pool.
The GPU upper alias of the graph uses AP2 data leaves; firmware has a distinct
AP1 mapping. A tiling pass requires retirement and nonzero GPU writes in both
the pool and tile map. Even that is not an exact vertex/fragment pixel result.

The reconstructed draw inputs remain experimental: dedicated 64 KiB zero
preemption buffers, single-cluster topology, compact VDM address 0x18000, and
zero optional descriptor fields. The fragment job is not submitted yet.
The graph grew to 512 KiB to hold those buffers and independent metrics state.

The first actual submission (20260907T175554Z, boot
211d3180-5574-4c0a-8929-81f639dff3af) preserved Linux and produced a firmware
assertion: non-sequential stamp updates, expected DAG index 0 but job stamp
0x100 selected index 1. It failed before GPU execution, with context-6 submitted
and retained generations still zero. The preceding barrier had registered the
queue at stamp zero. The opt-in TA path now seeds that barrier's +24 stamp to
0x100, matching InitPB and the following TA job. The ordinary queue-only gate
keeps its original zero stamp.

The corrected stamp retry (20260907T175835Z) passed scheduling and issued a
71-entry GPU register list (65 host entries plus firmware additions), then
triggered reason-3 recovery/IRQ 35. Context 6 advanced to submitted generation
1; its completion stamp stayed zero. The pool/tilemap acceptance check did not
pass. A bounded 5 ms observer now preserves banks 0..7 of the same M4 MMU fault
registers used by the existing compute observer, restoring the bank selector
after sampling. It does not acknowledge the interrupt or alter firmware.

The next run, with the launch observer (boot
4c3914c1-23ce-4ff2-9902-e9ba97396e26), lost SSH connectivity while insertion
was outstanding. No post-run diagnostic was retrieved, so the precise stop
point is unknown. Extra direct reads of the VDM, tile-map and PB-base registers
have been removed from the observer; it now reads only the existing fault/IRQ
diagnostic interface. That reduced observer is compiled but not yet tested.
Capture kernel output over a separate live SSH stream for subsequent tests.

## 2026-09-07 tiling and shader-entry diagnostics

The expanded graph and full 65-register TA producer now run on J713. Following
queue barriers, InitPB and TA retire with stamp 0x100, ring read pointer 3 and
busy zero. The firmware issued 71 registers including its six-word suffix.
The compact parameter-page IDs fixed writes into the owned 4 MiB pool; 38
nonzero bytes were observed. This is parameter-stream progress, not proof of
vertex execution. Tilemap/TPC and the expected color target remain unchanged.

Two separate markers now distinguish the captured vertex body from its state
loader. The vertex body gets a 40-byte store prefix, preserving the complete
86-byte captured shader and the following allocation. `--render-loader-marker`
relocates the first straight-line ESL to unused space within its own allocation
and prefixes a second store. Absolute binding and shader addresses are retained.
The readback words start as 0xcdcdcdcd, with expected values 0x56535441 (vertex)
and 0x45534c31 (loader). Neither marker was observed in runs 191812Z/192214Z;
Mandelbrot and the queue barriers still passed. Their logs and manifests are
in `validation/`. Shader execution remains unproven and no fragment job has
yet been submitted. All live runs must arm the mandatory 60-second watchdog
before inserting the module; `tools/run-mandelbrot.py` does this and saves the
verification in its manifest.
