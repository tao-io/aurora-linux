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

### Reuse retired render capacity across geometry changes

Render owners displaced by a fully retired replacement can now retain their
command/context and USC allocations when the next pass changes dimensions in
the same GPUVM. Geometry backing grows as needed; logical sizes, descriptors,
and complete reset semantics remain unchanged. Latest firmware-owned records
and cross-VM allocations are not eligible for reuse.

ARM64 module 412639d49f1c compiled successfully. Guarded full unmodified 0 A.D.
trace replay 20260911T114611717091Z captured calls 172530, 179895 and 187257;
all three frames are pixel-identical to the previous M4 output, with no API or
shader errors, and temperatures peaked at 52 C. This is prior-driver parity,
not pixel identity against llvmpipe.

Guarded actual game test 20260911T114803313970Z passed 180 seconds, with 384
thermal samples peaking at 53.5 C. The final scene and shadows were inspected;
HUD remains 2 FPS. CPU-clock samples across all cores attribute 1.70% to the
two render workers, versus 3.12% in the logging-only profile. Retained logs
record 4938 reused and 1212 newly allocated passes across desktop clients.
The cold compute readback failure remains unresolved and is independent of
this render-only change.

## Host command phase timing — 2026-09-11

Existing job-retirement logs now include last_prepare_ns, last_active_ns and
last_cleanup_ns, measured with CLOCK_MONOTONIC. These describe only the last
command in a packet. Active time begins before queue publication and includes
firmware scheduling, GPU execution and completion polling; it is not GPU busy
time. Cleanup measures the retired-owner replacement before signaling the
packet fence. No timeout, submission order or ownership rule changed.

J713 experiment133932 command-phase-timing-0ad-baseline passed180seconds under
native Hyprland/Zink with ordinary batching, compressed assets and postprocessing.
384 thermal samples, peak51.5C; final screenshot inspected with correct buildings
and shadows. Module7752732d, qualified Vulkanad40/Gallium9a5e. Across7102
single-command packets, preparation totaled32.018s and cleanup0.292s. Latest120
main-thread frames averaged2.015FPS. This qualifies instrumentation, not a speedup
or a fix for the separately recorded completion/visibility defect.
Workspace tools/gpu-phase-summary.py analyzes complete retained kernel logs
without comparing printk and CLOCK_MONOTONIC as if they were the same clock.

## Non-timeout fault evidence (2026-09-11)

Move the existing retained firmware/queue/render snapshot from the timeout-only
branch to the common active-job error path. MMU/firmware EIO failures now request
the same snapshot before fail_job; allocation/read failure does not replace the
original error or release DMA owners. No new MMIO or application buffer reads.

ARM64 module build passed. Guarded 180-second actual 0 A.D. control
`20260911T135629953009Z-restored-scratch-reset-0ad-control` passed, 385 thermal
samples, peak 51.75 C; final buildings/shadows visually inspected. This tests
normal operation, not a deliberately induced EIO snapshot. The rejected private
scratch-clear omission is fully reverted; it faulted in experiment134701.

## J713 maximum performance state (2026-09-11)

`asahi.g16_pstate` selects a fixed firmware performance state from 1 through 9;
the development default is 9 (1,470 MHz), explicitly requested for monitored
tests. `1` selects 338 MHz. This is fixed performance policy, not an adaptive
load/thermal governor. Preserve the bounded watchdog and external temperature
monitor until GPU thermal management is implemented.

`g16_power.rs` records the native J713/25G83 compacted frequency, core voltage,
SRAM voltage and auxiliary frequency metadata. Source witness:
`20260911T061054Z-fill-active-fwtrace/executing/firmware-initdata-main.bin`,
fields +fc8, +1008, +1408 and +1808. Core-position count +fc0 remains 10.
The constructor explicitly initializes policy bounds, outputs and initial
target, leaving current state zero for the actual firmware transition. No
foreign pointers, captured live controller history or PLL register writes.
The firmware/PMP performs voltage/frequency sequencing; each runtime poll
validates the hardware state against the configured ceiling. DRM frequency
metadata uses that same ceiling. Unported controller outputs are fixed by
this policy; this does not establish that native GPU thermal control works.

ARM64 module build passed; offline execution of the unchanged 25G83 selector
with these initial inputs selected state 9 with full duty. Guarded experiment
`20260911T192432058400Z-supertux-max-pstate` passed a 20-second SuperTux run
under native Hyprland/Zink. Both SGX +e01000 and the firmware current-state
field reported 9. Same installed Mesa6fbaeadc3f14, inline uniforms and opt-in
UBO prefetch as the preceding 338 MHz run: 29.948634 FPS versus 13.660367 FPS.
Measured 560 completed SDL swaps / 18.698682 seconds after 10-second startup
exclusion. Original baseline was 5.050340 FPS (5.93x overall improvement).
83 thermal samples, peak 54.25 C; watchdog60 and the 75 C cutoff remained
active. Final frame visually inspected: scene, textures and desktop intact.
This short run is not long-duration thermal or stability qualification.

## Bootstrap command mapping invalidation (2026-09-11)

Quickshell's direct Vulkan backend exposed stale context-zero command
translations: compute job115 faulted reading an earlier job's auxiliary VA
0x7104998000 in ASID4 while the new command named ASID5 and auxiliary
0x71024f4000. A separate OpenGL/Zink shell run reached the desktop but its
third independently created health-probe VM faulted on an old CDM address
in ASID6 while the current job used ASID7. Both followed reuse of bootstrap
command VA0x7000060000 after the prior GEM backing had been dropped.

The permanent low root never obtains a VmBind, so VmInner::slot returned
None. KernelMapping teardown removed its PTEs and freed the GEM pages without
invalidating the still-installed context0 translation. Record its explicit
fixed slot0 in the M4 UAT constructor; existing teardown/remap invalidation
now sees the actual context. Dynamic VM bindings and other platforms retain
their previous behavior. No extra command retention pool or global flush
is added to submission.

ARM64 module build passed. Module244b88c8aefce601f2c9be2626fe3dba2590fa32e0b0e11de486d5b1bb86a3a4
loaded on a recovered boot with no previous Asahi module loaded. Guarded
`20260911T194615186543Z-quickshell-vulkan-bootstrap-tlb` passed20 seconds,
61 thermal samples, peak53.75C, repeated compute checks and native KMS.
Quickshell's Qt RHI reports Vulkan on Apple M4; process owns Asahi DRM fds
and loads the private Vulkan driver. Panel and background final frame visually
correct. Watchdog60/thermal guard unchanged, normal max GPU state9.

## Retain retired parameter-page payload (2026-09-11)

Same-VM render reuse no longer clears parameter-page payload. Allocator,
scene metadata, private shader memory, completion records and preemption state
still receive their full reset. New backing remains zero allocated. Only an
owner displaced by a fully retired replacement is eligible for reuse.

ARM64 module a7bfea0f8c93 passed guarded native desktop experiment
20260911T213016244262Z-quickshell-retain-pb. All 323 thermal samples were valid,
peak54.5C. Warm R8/RGBA8 atlas readbacks checked310410bytes with zero mismatches;
menu scanout was visually inspected. Six-second cursor motion measured59.76FPS
hidden and30.08FPS open: this is a preparation-cost improvement, not60FPS yet.
The open interval's551jobs spent1052.22ms preparing and1885.12ms active, compared
with1161.80ms preparation across555jobs in the prior full-clear comparison.
Private-memory omission and persistent-UMA experiments are NOT qualified.

## Completion checking and compositor scheduling (2026-09-11)

The completion worker immediately checks the already-published fragment after
observing TA retirement instead of unconditionally sleeping100us first. This
can recurse only once per poll: the fragment branch does not recurse. Preserve
one complete GPU timestamp pair per retired render pass; redundant per-stage
stamp/queue logs now require KTraceCh. Completion and ownership checks remain.

ARM64 module85cf022a200e passed guarded213545-completion-check startup and warm
R8/RGBA8 atlas oracle (310410bytes, zero mismatches). Identical six-second motion
still measured59.77FPS hidden/30.10FPS open; active time dropped only modestly
(1862.72ms across551openjobs), so do not attribute a major gain to polling.

Within the SAME guard, enabling Hyprland render:new_render_scheduling yielded
59.38FPS hidden/54.95FPS open. The compositor's native-fence scheduler overlaps
rendering with presentation instead of falling to half refresh while waiting.
Keep this supported option in the supervised Omarchy config. Same resolution,
Mesa binaries, rendering, GPU clock and conservative CPU caps; no software
fallback, drawing reduction or quality change. This is approximately55FPS,
not a60FPS qualification yet. Complete experiment evidence is retained by the
workspace tools under213545-completion-check.

Qualification boundary: the213545guard's end-of-run wrapper failed its obsolete
assertion requiring the now-optional per-stage log. Rendering/health checks
passed,565thermal samples peaked55C, but the overall guard result is FAIL and
it rebooted after collecting evidence. The wrapper now requires a complete,
nonzero GPU timestamp pair instead. This must pass in the follow-up run before
claiming a complete qualification of the completion-log change.

Follow-up214231-normal-scheduler passed the corrected wrapper, all50health
checks and522thermal samples (peak67.5C). Starting with normalCPU ceilings
4104/2892MHz measured59.71FPS hidden/41.97FPS open across ten-second intervals;
higher CPU clocks did not improve pacing. Caps were lowered back to1860MHz
(the kernel rounds the P cluster to1800MHz), with all guards still active.
The user reports mostly60FPS with occasional severe dips; these spikes remain
an investigation target, not a stable60FPS claim. Preserve new scheduling and
use the cooler caps for further timing work.


### 2026-09-11 native presentation timing history

The development DCP KMS module exposes the last 256 completed presentation
records through `m4_present_history`. Each record contains CLOCK_MONOTONIC
submission/completion timestamps, sequence, framebuffer ID and status. A
separate short spinlock protects the history snapshot; readers never wait for
a native swap or copy scanout pixels. This is diagnostic history capacity,
not a buffering or submission limit.

ARM64 module build passed. Guarded J713 run
`20260911T215216385951Z-quickshell-present-history-evidence` passed 101 health
checks and 1,037 thermal samples, peak 56 C, zero native presentation failures.
DCP module SHA-256: `6d8454cfee9e87548c24333e5288f89df4f3b80b48e5bd96d3ac996f7258af97`.
The ten-second menu-hover interval had median 16.665 ms, p99 33.437 ms and
maximum 33.495 ms. A 20-second continuously moving-pointer test with five menu
opens caught five gaps over 50 ms (maximum 66.763 ms), predominantly before
native submission; maximum observed cursor IPC latency was 82.819 ms.
The history reader took at most 0.813 ms. No missing history records or DCP
failures occurred. This reproduces a smaller stutter, not the user's estimated
200 ms worst case, and does not establish a steady 60 FPS desktop.
