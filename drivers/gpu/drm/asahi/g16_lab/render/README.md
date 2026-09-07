# J713 vertex/fragment bring-up

Status: Linux buffer and packet preparation implemented and exercised on J713.
No Linux vertex or fragment execution has been established. Existing Mandelbrot
compute validation remains separate.

`stage=render-prepare` imports the 34 encoded capture mappings, allocates a
4 MiB parameter pool, generates its 128 virtual 32 KiB page IDs, initializes
the hardware parameter-manager state and builds TA/fragment job envelopes and
SKU streams. It exports a 64 KiB preparation snapshot as
`/sys/kernel/debug/g16_lab_render_prepared`. Module removal releases all of
these allocations. This stage does not publish UAT mappings or submit work.
It builds private 42-bit UAT trees and verifies every leaf against its allocated
Linux page: 475 user leaves in ten table pages, plus four firmware leaves in
three table pages on the tested J713 run. The firmware tree contains only the
new command graph; it still needs the shared firmware mappings before use.
Captured-resource permissions currently use the working compute AP2 executable
tuple provisionally, rather than claiming captured render PTE evidence.
The register blocks currently contain only their stage's initial register;
the remaining render registers, pool/queue bindings and submission are pending.

`g16_render_ta.h` now translates the native TA register producer, including its
65-write baseline, conditional registers, pointer masks, replicated register
values and mode word. It takes an offset-labelled descriptor plus explicit
platform/PB inputs and never dereferences native CPU pointers. Host tests cover
its alias writes, masks and mode encoding; the header also compiles in the
kernel build. This generator is not yet called by `render-prepare`: the
descriptor's render geometry, private buffer addresses and platform defaults
still need to be supplied from a reconstructed draw. Fragment register-value
generation remains to be implemented.

Prepare the firmware input with:

```sh
python3 tools/pack-render-resources.py render/reference /tmp/render-reference.bin
sudo install -D -m 0644 /tmp/render-reference.bin /lib/firmware/g16/render-reference.bin
sudo insmod ./g16_lab.ko stage=render-prepare
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
This is CPU preparation evidence, not a GPU execution result.

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
