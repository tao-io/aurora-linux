# J713 vertex/fragment bring-up

Status: reference preparation only. No Linux vertex or fragment execution has
been established. Existing Mandelbrot compute validation remains separate.

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
