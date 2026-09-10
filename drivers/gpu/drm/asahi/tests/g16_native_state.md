# M4 render state comparison (J713, macOS 25G83)

The M4 adapter previously replaced userspace ISP merge limits with the geometry
macro-tile count and zero, and discarded color tilebuffer capacity and render
dimensions. `hk_queue.c` supplies the merge limits as IEEE float bits, and the
common Asahi render path forwards them to registers 0x15130/0x15138. The M4
register map is the same for these fields. Preserve the bits without kernel
floating-point operations. Color tilebuffer storage uses 2 KiB blocks, rounded
up from sample bytes times utile dimensions (the current M4 path accepts one
sample). Zero remains valid for depth-only rendering.

Matched native Metal texture-loop captures on the owned Apple M4 / Mac16,12:

| Render size | ISP merge X/Y bits | Packed dimensions |
| --- | --- | --- |
| 128 x 128 | 0x3c5db3d9 | 0x0000008000000080 |
| 1024 x 1024 | 0x3addb3d9 | 0x0000040000000400 |

Both native captures wrote 0x1c to TA/fragment registers 0x101e0/0x101e8 and
8 to 0x1a0f0/0x1a0f8. These match the existing compute setup and Asahi's other
engine producers. They were also inadvertently zeroed in the M4 render path.
The exact 25G83 native producers are generateRegisterListFor3D at 0x89c162c
and generateRegisterList at 0x89cbcb0; corresponding portable source is in
`g16_lab/render/g16_render_fragment.h` and `g16_render_ta.h`.

Private raw captures are retained outside Git as 20260910T193311Z (1024) and
20260910T193422Z (128). Register-list SHA256:

- Full 1024 RunFragment: 8503abafb76853fa4c21aae1b78db99cb007382874f9825524b4bfd0761cb7e3
- Full 128 RunFragment: 45a2adc2684cf9217e0e732a8244e7c04b462a04a47c595c90592d986dee83af

The native baselines and gated render captures verify every output pixel. The
capture dwell changes timing; its duration is not baseline performance.

Validation of this adapter correction: ARM64 module build, native-state wire
fixture, existing geometry/register/dependency fixtures. On J713 with mandatory
watchdog and thermal guards, the 128 texture-loop draw passed both full pixel
reads. Its preceding first no-loop draw still had the separately tracked cold
immediate-read failure (second read correct); the 1024 texture-loop draw still
timed out. This fixes missing state forwarding, not the unresolved completion,
large-shader or 0 A.D. correctness bugs. GPU clocks were unchanged.

The auxiliary framebuffer controls also follow the common Asahi path:
ISP control 0xc000 for the currently admitted floating depth-bias path,
and auxiliary control 0x100000. Both were previously zero. Native M4 has
0xc001/0x100000; its extra low ISP bit is not inferred from this capture.
The M4 adapter still rejects integer depth-bias requests. The auxiliary buffer
is already larger than the native 32 KiB allocation.

This correction builds for ARM64 and passes the wire fixture. Hardware run
20260910T201157 still times out in the 1024 texture-loop draw. The subsequent
128 texture-free arithmetic control retires with an exact second read, but
its first read remains incorrect. Neither result qualifies full correctness.
The separately tested 20-second timeout and power-policy candidates failed;
they are not part of this change.

The matched captures also carry RunCommand+4 counters in StartVertex+1b8
and StartFragment+1d8, and render mode 1 in RunFragment+c84,
StartFragment+1e4, FinalizeFragment+b1 and FinalizeVertex+8d. Preserve the
full 64-bit counter and all mode mirrors. The 25G83 processRender producer
at 0x897b49c..0x897b4c0 selects modes 1/2/3; the former zero-fill was not its
normal path. This mode is separate from ISP register 0x10038. Mode-2 flags
in the run commands remain clear for this single-pass path.

Wire regression tests and ARM64 compilation pass. Hardware runs
20260910T205140 (128) and 20260910T205159 (1024) retain the cold first-read
mismatch and large-fragment timeout respectively. Small mode2 passes both
reads. These are command-format corrections, not a claim to fix the stall.

The runtime also now advances a 64-bit render command counter pair instead
of reusing 1/2 for every pass. Fragment precedes vertex in the pair, matching
queue/render.rs and the captured native pair (1/2). Counter exhaustion is
rejected before publication. ARM64 build passes; guarded 210048 still times
out at 1024 (peak 50.5 C), so monotonic counters alone do not cure the stall.
