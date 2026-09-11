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

RunTA+906 and RunFragment+c4e also now request firmware cache maintenance
of their retained UMA descriptor. Both native captures set this byte to1;
25G83 TEXT+21144/+223fc pass it to2af18, which calls3e8c8 for0x98bytes.
The 58134/58338 path performs firmware CPU DC CIVAC followed by DSB SY.
The compute adapter already supplies the equivalent RunCompute+872 flag.
Packed neighbor offsets are covered by the native wire fixture. This does
not add application-CPU framebuffer copies or change GPU clocks.

ARM64 build and four native-state tests pass. Guarded210501 small retains
firstmode0readbad/secondexact and bothmode2readsexact (peak50.25C).
210523 large stilltimesout (peak49.75C). Neither establishes0ADcorrectness.

The third TA preemption buffer is now owned alongside the first two and
bound through register1c950 with bit50, matching the native descriptor f08/f10
and common Asahi preempt_buf_3. It is zeroed before reuse and retained for
the pass lifetime. A16KiB allocation covers the single-cluster record;
the earlier two64KiB areas are retained. The existing low-address reserve
is retained pending qualification of all remaining bindings.

Native wire tests and ARM64 compilation pass. Updated C/Rust state fixtures
match all19,392bytes across three geometries and both stages. Guarded211520
large stilltimesout(peak50.75C);211701small retainsmode0firstreadfailure,
exactsecondread,andmode2bothreadsexact(peak50.5C). No fullgamefixclaimed.


Render timestamp port: native25G83 TA and fragment
SKU records each contain a0x4c-byte start timestamp, WFI, and a0x4c-byte end
timestamp. Their pointer fields address RunCommand cells, not raw result
storage. The pass owner now retains distinct start/end cells for each stage;
the native records point through TA8c8/8d0 and fragmentc10/c18. Restart offsets
and attachment-finalize flags account for the longer sequence. Timestamps
are logged as raw ticks; UAPI timestamp requests remain rejected until their
separate destination/lifetime path is implemented and tested.

Offline verifier compares both full timestamp records, WFI and restart
positions byte-for-byte with matched193311 native sequences (identity pointers
and UUID taken from the input). Both stages match. Existing six dependency/
geometry/state tests and ARM64 build pass. Guarded222429 small renders populate
ordered timestamps for both stages, without a GPU fault (peak51.5C); the
known cold first read remains stale, so this is not a correctness pass.
Guarded222452 full1024 loop20 still times out in fragment (peak50.75C).
Both fragment timestamps are populated while retirement stamps remain zero.
Firmware trace shows two execution intervals and reason5 quiescence. The
end timestamp records a partial/preempted interval too; it is not proof of
a complete render. No Vulkan timestamp-query support or 0AD fix is claimed.

UMA allocation defaults: retain native G16 min/max DUPM 1/2 for both stages
(registers d418/d428), and emit fragment a5a8 instead of inheriting the first
4x4 lab's disable_spill_registers diagnostic. The no-override fallback is
6000400020 in the native TA/3D producers. Matched193311/193422 captures instead
supply explicit capacities (TA a900400020, fragment d500400020), but both
confirm d418/d428=200000001. The G16 virtual defaults at89c4438/89c4444 return
2/1. Do not claim our fallback equals the captured dynamic capacities.

Four native-state/geometry tests and ARM64 build pass. Guarded224037 small
has the same known firstmode0read failure; secondread and mode2bothreads exact,
no GPUfault, peak52.5C. Guarded224054 large still times out in fragment,
bothreads incorrect, peak50.5C. Saved allfault evidence and reboot recovered.
This removes inherited lab state; it does not fix the long-render stall.

### Opt-in high-volume logging

Normal desktop submissions no longer print every G16 firmware ktrace record or
GEM bind. The existing `debug_flags` bits enable these diagnostics: bit17
(`KTraceCh`, 0x20000) for firmware traces and bit0 (`Mmu`, 0x1) for binds.
Trace records are still drained and validated, and trace loss remains reported.
Fault reports and per-job retirement records, including monotonic timestamps
used by the watchdog guard, remain unconditional.

The triggering 180-second 0 A.D. CPU profile recorded 1,199,592 firmware trace
lines and 494,309 bind lines, with journald accounting for 7.69% of system-wide
CPU samples across all cores. Profiling and bounded run evidence are retained
in workspace experiment `20260911T112352341126Z-default-0ad-cpu-profile`.
The game finished its 34 health checks; the outer guard failed because offline
report generation was mistakenly run on-target with a 20-second timeout.
No GPU fault was observed. ARM64 module compilation passes for the log gating;
hardware validation is recorded below once completed.

Guarded test `20260911T113708594165Z-opt-in-logging-0ad-profile` passed the
180-second actual game run and all 35 health checks; 387 thermal samples peaked
at 53.5 C. The final image retains the scene and shadows (HUD still 2 FPS).
Logs fell from 186 MiB to 7.7 MiB, and journald fell from 7.69% to 0.45% of
all-CPU samples. This reduces logging overhead; it does not fix cold visibility
or establish faster gameplay. Both profiles retain their exact installed hashes.
