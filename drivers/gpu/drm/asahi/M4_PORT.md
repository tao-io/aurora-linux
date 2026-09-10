# M4 integration into Asahi

The completion target is parity with the old driver: correctly rendered,
GPU-accelerated Hyprland and SuperTux through Asahi and Mesa Vulkan/Zink.
A first verified Vulkan program is an intermediate milestone, not completion.

Current result: the owned Asahi startup diagnostic executed its one-thread GPU
store in `compute-store-8`. Userspace submission, Vulkan, Hyprland and SuperTux
through this new runtime are still pending. The sections below retain the
chronological bring-up record; earlier failure boundaries are not current claims.

The `m4-asahi-port` branch imports the complete Asahi driver and UAPI from
Aurora `drm-agx3`, commit `4ac2664258f370cf7b03327da742b5885f91ca0b`.
The source import is `ebac9112f313`; `ab731d9f3dfb` adds the fourteen upstream
binary fixtures hidden by the inherited ignore rule. All 151 upstream driver
and UAPI files match their original Git content hashes and modes across these
two commits. Transport dependency integration is `60e61fd5fdf7`.
RTKit retains the existing CoreAnalytics support and initializes
adopted sessions before receive interrupts, alongside the new Neo transport.

The parallel Mesa worktree starts from Aurora `agx3`
`e58d46899eb1f351863bccab4b6e8c87ed32b95e`. The previous lab kernel branch
ends at `33f5bc310610`, and the lab Mesa checkpoint is `c2cb7a5934b`.
`g16_lab/` remains available as evidence and does not participate in the
`CONFIG_DRM_ASAHI` build. M4 userspace execution remains disabled; only the guarded startup diagnostic runs.

## First transferred implementation

`g16_render.rs` implements M4/25G83 single-sample geometry, hardware parameter
buffer descriptors and TA/fragment register program framing. Its addresses and
register values come from its caller. It owns no memory or hardware state and
does not reproduce the lab's fixed allocation graph or submission worker.
The M4 firmware command sizes are TA `0x940` / fragment `0xca0`, distinct from
Neo's TA `0x9c0` / fragment `0x2240`. Register program framing also remains
generation specific.

The Rust host tests check executed J713 geometry, both parameter-buffer UAT
root selectors, reserved bits, untouched attachment apertures, and rejection
without partial writes. They establish encoder parity, not execution through
the new driver. The two Mesa reciprocal/select fixes are carried separately
without the lab device initialization path.

## Normal resources and firmware identity

J713 now has a disabled `apple,agx-t8132` node and a disabled mailbox provider
in `t8132-j713-gpu.dtsi`. Its named regions are `ttbs`, `pagetables`, `handoff`,
`shared-l2`, `fw-text` and `fw-data`. The paired `m1n1/m1n1-asahi` worktree,
also on `m4-asahi-port`, fills these from the current ADT, marks them no-map,
and reserves them through intermediate boot stages. It converts segment
metadata to FDT byte order and publishes the version marker last. It does
not power or start the GPU and leaves the device status unchanged.

`g16_resources.rs` consumes this normal device contract. It requires the J713
board and single mailbox topology, unique phandles, nonoverlapping aligned
42-bit memory ranges and the declared segment order. It accepts relocated
physical addresses. The private `/chosen/g16-lab-*` properties are not used.
The mailbox aperture and interrupt descriptions come from the J713 ADT;
interrupt delivery through this provider passed the guarded discovery test below.

`g16_firmware.rs` maps only the reserved loaded text, hashes a private copy
with the kernel SHA-256 implementation, and identifies the lab's firmware ABI
by text contents and segment geometry. The mapping is released before
returning the identity. It performs no firmware patch or GPU register access.
The currently pinned evidence is:

- Source: `/home/council/dev/ESP32KeySim_m4/boot-attempts/g16-resterring/g16g-fw.bin`.
- Text size: `0x60000`; data size: `0x13c000`.
- Text/data VAs: `0xfffffc0000000000` / `0xfffffc0000060000`.
- Embedded string: `RTKit-3255.160.4.release`, text offset `0x5a2b0`.
- Text SHA-256: `704f1b4cd74c036ea85bdc191bdbe766185b5b1cfa8d4af591b065890d1ec42c`.
- Normalized SHA-256: `5f65f61458725470b4b0c02028477bf2ea573afb4e0c496ef995e96affb0d86c`.

The first integration boot (`d25b18d9-7aaf-4c2d-9980-3ee4a1412830`) showed
why the complete loaded text hash is not a stable firmware identity. Its text
differs from that reference in exactly 16 bytes, the 8-byte STKG and PACE
boot-argument values. The code and every other byte match. The verifier now
checks the boot-argument table offset/size and those two key/length headers,
zeros only those two values in its private copy, and hashes the result.
Instruction bytes and the other boot arguments remain covered. Seven host
tests pass, including malformed headers and preservation of instruction
changes; the normalized reference and live captures have identical hashes.

The reference text hash and embedded string were checked on 2026-09-09. No Apple
firmware bytes are added to Git by this port. This selects a known firmware
ABI; it does not replace iBoot signature verification or establish Linux
ownership of the firmware's state. The new probe acquires power and reads SGX
identity after matching firmware, then releases power and returns ENODEV.
It then performs bounded ASC management discovery; it does not register DRM.

Validation: full ARM64 Image/modules and m1n1 stage-2 builds pass. The binding
passes dt-doc-validate, and both the source DTB and generated handoff DTB pass
the T8132 binding check. The actual m1n1 FDT producer passes 13 host cases;
the generated result passes the Rust resource validator. Firmware identity
tests cover physical relocation, unknown digests and incompatible geometry.
The new bootloader and kernel have now booted on J713. The initial guarded
probe correctly refused the unnormalized identity. The firmware comparison
was collected separately; that capture helper's unbounded-size MTD read hit
an alignment fault, so the read was changed to an explicit aligned size and
a clean reboot is required before continuing GPU experiments. No shader has
executed through the new runtime yet.

After reboot, normalized identity and acknowledged GFX power passed on J713.
The first cleanup exposed a Rust exclusive-MMIO reservation made under a
detached OF descriptor, then released through the global tree. The wrapper
now reserves through the global MMIO tree too. Two consecutive probe/remove
cycles on boot `04d0137e-c9a0-4b4b-89da-e86de7b40ad1` read version
`0x0a021100`, counts `0x0011010a`, core mask `0x3bd`, and completed without
resource-release errors. The watchdog was verified at 60 seconds and the
six-channel thermal monitor peaked at 52.25 C. Evidence is under
`_provenance/asahi-pivot-20260909/hardware/power-probe-3-repeated/`.

The next probe stage uses the normal RTKit discovery-only client with mailbox
interrupts. It claims power first, enables ASC, waits only for HELLO/EPMAP,
then stops ASC before releasing RTKit and the PMP vote. It does not start
crashlog or application endpoints, allocate firmware-visible host memory, or
send initdata. A firmware session must not be retried without a reboot yet.

Discovery passed on boot `04d0137e-c9a0-4b4b-89da-e86de7b40ad1`: HELLO
`0x00100000000c000c`, protocol 12/12, endpoints 0, 1, 0x20 and 0x21.
The normal mailbox provider delivered three receive interrupts. ASC stopped
before releasing the transport and power. No INIT power message was needed
for this passive exchange. Peak monitored temperature was 51 C; watchdog
was verified at 60 seconds. Evidence: `hardware/discovery-1/` under the
pivot provenance directory.

## Runtime integration boundary

The outer power hierarchy is now represented as GFX TVM -> PMGR GFX, with
both nodes disabled pending guarded integration tests. The TVM provider owns
only bit 0 of `0x380760800`. The GFX PMGR provider requires that parent and
enables automatic gating after reaching the active state. PMP's inner AGX
power request is a separate firmware operation, after the outer domain is on.

PMP requests now wait for the serialized device-state reply instead of treating
mailbox enqueue as completion. The J713 coexistence log at
`_provenance/omarchy-m4-display/20260909-gfx-coexist/dmesg.log` records
`0x0020f50100000000` and `0x0020f50000000000` requests both acknowledged by
`0x0021000000000000`. A timeout or unexpected reply prevents further requests
in that session: the wire protocol has no request ID to distinguish a late
reply. Three host protocol tests and the ARM64 object build pass; this request implementation passed repeated J713 power acquire/release tests.

That same log records `magic_fw == 0` at the old driver's pre-submit boundary,
with the protected handoff export pointing to the correct host magic. M4 must
therefore implement its lazy handoff initialization, not wait unconditionally
for the older AGX2 magic acknowledgement or assume Neo's absent-peer policy.

Use `drm_gpu::DrmGpu`, the common DRM file/GEM/syncobj layer and queue contract
for the M4 backend. The Neo backend supplies useful implementations and
evidence, but its firmware layout, dual-ASC lifecycle and generation-specific
memory policy cannot be selected for T8132 by changing a chip check.

Transfer the following in order, with ownership and error unwinding implemented
at each boundary:

1. Identify the J713 loaded firmware and device-tree resources; bind the M4
   firmware ABI to that identity. Translate the lab's verified startup and
   handoff protocol into owned RTKit/MMU resources.
2. Implement the M4 VM/context lifecycle in the common memory architecture.
   Map and retain GEM backing through job retirement, with a defined DMA/cache
   contract. Do not reuse one global lab context or rebuild every client page
   table for every bind/submission.
3. Construct M4 initdata, parameter buffers, TA and fragment work from owned
   allocations. Complete the register producers and firmware microsequences
   using the recorded vertex/fragment execution evidence.
4. Connect queue scheduling, completion fences, dependencies and recovery.
   Prove offscreen pixels, concurrent client isolation and teardown before
   exposing a desktop to the new runtime.
5. Port normal Honeykrisp M4 admission and graphics state, then validate
   DMA-BUF sharing with the existing native display driver and Wayland/Zink.

Successful lab compute, vertex/fragment shaders and desktop rendering are
hardware evidence for their tested paths. They do not qualify a new driver
runtime. In particular, the lab's async GPU fault is unresolved and its global
VM/cache behavior must not become the architecture of this port.

## Build and test

Use the known J713 16-KiB configuration, enable `CONFIG_DRM_ASAHI=m`, and retain
the existing thermal, MTP, storage and display configuration. The initial build
uses Rust 1.98.0, LLVM 22.1.8, and local version `-dirtyroom-asahi1` in a separate
output directory. Build the complete Image/modules to catch RTKit and mailbox
link dependencies in addition to compiling the driver object.

The initial complete ARM64 `Image modules` build passed on 2026-09-09,
including `asahi.ko`, with release `7.1.9-dirtyroom-asahi1+`. Relative to the
previous J713 config, only Asahi, its required DRM/Rust dependencies and the
local version changed. The build output is `build/linux-asahi-j713` relative
to the collection root, and its full log is `build/asahi-kernel-build.log`.
The companion fresh Mesa AArch64 Vulkan/Zink build also passed. See its
`docs/m4-asahi-port.rst` for the outstanding upstream compiler test failures;
successful compilation does not establish runtime support.

The pure M4 encoder tests require no kernel or GPU:

```
rustc --edition 2021 --test drivers/gpu/drm/asahi/g16_render.rs -o /tmp/g16-render-tests
/tmp/g16-render-tests
```

Hardware experiments must keep the workspace's verified 60-second watchdog,
all six PMP temperature channels, enabled CPU thermal policy and conservative
test clock caps. Do not bind Asahi while the lab owns the GPU. The integration kernel has booted and completed power and management tests;
no shader or DRM client has executed through the new runtime yet.

## Reserved table ownership check

The common `UatPageTable` now supports retained CPU mappings for inherited
no-map table reservations. Initial parent links are validated against those
reservations before adoption. Driver-allocated descendants are detached before
being freed; reserved pages are never returned to the page allocator. The
noncoherent path publishes modified PTE spans and freshly allocated tables,
not a complete page-table copy at each submission.

Guarded J713 test `hardware/reserved-tables-1/`, boot
`edb9c5a0-b906-4dad-95db-c7ce8095fef0`, passed two map/protect/unmap/drop cycles,
including a leaf-table boundary and the end of the kernel VA window. It checked
translations, RO/RW permissions, occupied-map rejection and complete restoration
of all inherited table bytes. The four occupied-map log messages are deliberate
negative checks. All TTBAT entries remained disabled and ASC was stopped during
the test. RTKit discovery then passed again. Peak temperature was 53.25 C.
This is CPU page-table/lifetime validation, not a GPU translation or shader test.

## RTKit boot and initialized DRM data

`hardware/rtkit-boot-1/` records successful common RTKit startup on boot
`072a56a6-1a0b-44d8-b22b-b460bfb9f97c`: firmware crash storage at physical
`0x10001e80000`, size `0x8000`, is bounded inside the admitted firmware-data
reservation and retained through transport cleanup. Its AP mapping is uncached.
Both IOP/AP running states and application endpoints 0x20/0x21 completed.
ASC stopped before RTKit freed its callback data and CPU mapping. Peak 52.25 C.
This uses the ordinary RTKit path, not Neo's crash-DVA echo behavior. No initdata
or GPU command was sent.

The imported DRM initializer previously returned success with uninitialized
`AsahiData` and disabled all Rust DRM data destruction to tolerate it. GPU and
Neo runtime slots now start as empty `SetOnce` values; the complete data and
work items exist before GEM construction. DRM callbacks return ENODEV until
the backend is published. Normal DRM data destruction has been restored.
This removes uninitialized-data access and constructor-unwind leaks; it does
not by itself resolve persistent runtime reference cycles or qualify Neo unbind.

`hardware/drm-init-1/`, boot `83de9a23-97b1-4c2d-96be-a75485e0cda5`, passed
two unregistered DRM/GEM allocation/cleanup cycles, both reserved-table cycles,
and another full RTKit/application-endpoint startup. Peak 51.75 C. The DRM
backend remains absent; there is still no new-driver Vulkan execution.

## Common UAT ownership and firmware handoff

The handoff lock now returns a bounded error instead of proceeding without
firmware exclusion. Its RAII guard releases AP interest on every exit path.
A failed handoff or firmware cache flush faults the UAT session; affected VMs,
GEM mappings and physical page-table backing are retained until reboot rather
than recycled while firmware may still reference them. This is containment,
not a complete hardware reset/recovery implementation. The host lock tests and
`hardware/handoff-guard-1/` verify ordinary acquisition and both timeout paths;
the injected failures use private CPU memory, not the live firmware region.

The M4 constructor now uses the common UAT/GEM/GPUVM manager, a 42-bit address
space, independently allocated user roots and ordinary context allocation.
Its identified firmware policy initializes current-context to 0xffff and
publishes AP magic/attention without requiring an immediate firmware magic
reply. TTBAT and handoff use uncached AP mappings; owned page tables publish
changed PTE spans. The constructor requires stopped ASC and teardown requires
quiesced hardware. M4 teardown retracts TTBAT entries before freeing roots.

`hardware/common-uat-1/`, boot `0a12e60d-dcbb-4b0c-a698-a9643a5f5fd5`, passed
two complete UAT/VM lifecycles. Two VMs mapped VA 0x100000000 to different GEM
pages with different permissions, obtained distinct roots and contexts 1/2,
then released mappings, slots and roots. The following raw-table check found
all contexts disabled and preserved inherited tables. RTKit boot then passed.
Peak monitored temperature was 52 C. This establishes CPU-side VM construction,
translation and retirement; no GPU page walk or shader has executed yet.

### Owned runtime, live common UAT (2026-09-09)

`owned-runtime-1` on boot `4b2682ec-e393-4e6f-b7af-d0dbcb09f694`
started RTKit with common UAT/context 0 still installed. Application endpoints
started and ASC stopped before transport/UAT/power cleanup. The 60-second
watchdog was verified; all six thermal channels were monitored, peak 52 C.
No initdata or GPU commands were sent in that test.

The first relocated-initdata run (`relocated-initdata-1`, boot
`d5bec735-fc7c-46f8-a42e-e7884ce34a5b`) reached the initdata receive routine,
then firmware faulted at PC +0x31748, FAR 0xfffffc2000000000, ESR 0x96000006.
Common RTKit decoded the crash log; Linux remained responsive, peak 51.75 C.
No GPU command was queued. Unmap detected the missing root mapping and faulted
the UAT session rather than recycling its backing. The machine was rebooted.

The loader installs the designated reserved shared-L2 link during RTKit startup.
M4 kernel-VM construction now adopts that same table before creating GEM
mappings. A conflicting root or detached nonempty table is rejected. This
preserves the common walker and allocator while honoring the firmware's boot
translation contract. Hardware acceptance after this correction is pending.

`relocated-initdata-2`, boot `1dac1462-919b-42f2-ad77-e1551d01d332`,
passed after adopting shared-L2. Firmware printed its G16G cold-boot messages
through the relocated GEM log channel and set control-ready=1. No GPU command
was queued. ASC stopped and common mappings cleaned up without errors.
Watchdog 60 seconds, all six thermal channels, peak 51.75 C.

Device-control testing now uses the three-index firmware ring and an explicit
Fender SRAM power-assertion generation. The startup no-op is published before
initdata, with subsequent commands using the rescan notification. Completion
requires consumer/CFI/producer agreement, ready state and 50 ms stability. The GPC state is checked against
the low boot-state cap. This stage still queues no GPU work.

`device-control-1` retired the startup NOP at indices 1/1/1 but timed out
on an incorrect host-side power-generation equality check. The old successful
fenced-noop evidence shows host/consumed pairs 1/0, 2/1, 3/2: the initial NOP
does not increment the latter. Power accounting is now logged separately from
the ring-retirement test. Firmware did not crash; Linux remained responsive.

`device-control-2` passed all three device-control retirements, including
50 ms stability at each step, with host/consumed generation pairs 1/0, 2/1,
3/2 matching the old M4 evidence. CRE save/restore diagnostics were readable
through the GEM log channel. Firmware stayed ready, the low GPC state was
retained, and ASC/VM teardown completed cleanly. No GPU command was submitted.

`queue-barrier-1`, boot `b511c365-4afe-4f52-a181-ad75b91d3506`,
entered the firmware scheduler but asserted with `Bucket overflow` and zero
accounting buckets. Linux remained responsive; ASC stopped and evidence was
saved before a guarded normal reboot. No GPU engine job was submitted.

The 25G83 import at firmware +0x31800 uses base 0x192db0 (not 0x192000).
Main +0xea0 therefore populates 0x193b48, and +0xee0 populates 0x193b7c.
The former enables queue accounting; the latter gates bucket initialization
at +0x3b614. Restore the paired configuration used by the successful old M4
fenced-noop path, retaining its own GEM save area. Idle-power-off remains
disabled through device-control, with low-pstate checks. This does not enable
a higher DVFS state or establish GPU thermal throttling.

`queue-barrier-2`, boot `987a6af4-b2cf-4c85-84d8-eb2cb4d79637`,
passed with the paired initialization. Independent GEM-backed pipe and queue
rings retired at 1/1/1; the queue returned idle and its context registered.
Firmware stayed ready through 50 ms stability, low pstate was retained, and
ASC/VM teardown completed. Verified 60-second watchdog, peak 52.5 C across
six monitored channels. This is a firmware barrier, not a GPU engine shader.

### Owned compute-engine qualification

The next startup check encodes a CDM terminate and its 25G83 firmware
microsequence. Command, stamps, context storage, empty private-pool metadata,
and global UMA table use common GEM allocations with independent canonical
firmware and allocator-selected lower-half GPU mappings. Kernel context zero
is only the initial engine qualification target; user submissions must retain
their own common VM bindings. No shader or Vulkan claim follows from terminate.

Retirement requires both independently produced completion stamps, ordered
nonzero timestamps, queue idle, and 50 ms stability. A recovery event, crash
or timeout leaves the entire runtime, GEM backing, mappings, PMP vote and
module code retained until reboot; merely halting ASC cannot retire GPU DMA.
The watchdog wrapper recognizes this retained-fault state and reboots after
saving evidence. A real reset/recovery implementation remains required.

`compute-terminate-1`, boot `11a9c3a2-a42d-40da-9fe6-cd902b5ae8b4`,
stopped responding and rebooted with the watchdog armed. Only startup and
one thermal sample (peak 51.5 C) survived; the journal has no module output.
The precise failing stage is unproven. The next run streams kernel evidence
back to the host while probing. Engine/MMU diagnostic registers are skipped
when GPC reports pstate zero: the working old compute path explicitly reports
the GPU powered down at that point and takes those snapshots after issue.

`compute-terminate-2`, `-3` and `-4` consumed the queue and wrote both stamps
(0x100), but produced zero start/end timestamps. They are failures, not GPU
execution proof. Linux stayed responsive and the guard saved evidence before
normal reboot with the watchdog armed. Run 3 added basic decoded firmware
KTrace: StartCompute/FinishCompute, stamp and event records appear without
recovery. Context-0 roots remain installed and power-generation remains 2/3
before compute, so there is no evidence that the preceding barrier consumes
the outstanding power assertion.

Run 4 corrected canonical aliases of shared command/state buffers to permit
both firmware and GPU access, including owned slot state and the UMA counter.
This matches the qualified old M4 object permissions but did not resolve the
zero timestamps. The firmware timestamp opcode reads engine timestamps then
writes CPU-visible cells; a firmware completion stamp alone remains inadequate.
Evidence is under `_provenance/asahi-pivot-20260909/hardware/` in the collection.

`compute-terminate-5` separates read-only GPU CDM backing from writable state;
the host wire tests pass, but hardware still returns zero timestamps and the
guard performs a normal reboot. Run 6 enabled scheduler KTrace and attempted
post-retirement Fender status reads. It stopped responding near the timeout
path and recovered to boot `f1765614-4559-4d4e-91ad-d906cf001c68` after several
minutes. The host saved about 6500 trace lines; only startup/one thermal sample
survived on disk. The precise last instruction is not established. Remove
those direct reads and restore basic KTrace; further snapshots use firmware
RAM. Neither experiment demonstrates GPU execution.

`compute-terminate-7` safely snapshots firmware RAM: the engine builder emitted
34 register entries, with valid owned low/canonical register-list aliases.
Timestamps remain zero. `compute-user-1` then bound an independent common VM
(slot 1, initial generation 1). This activated the GPU at the guarded low
pstate 1 and enabled its MMU, but faulted on a read of VA zero in bootstrap
context 0 (unit 0xd0, raw 0x800c01a111). The old qualified M4 code explicitly
maps the command register-list page into context 0 because hardware fetches
that list before switching to the client ASID. The next fix retains only that
owned command GEM in both VMs at its allocator-selected address, preserving
the separate context-0 UMA table. This is not a GPU execution success yet.

The one-thread store diagnostic uses dynamically encoded MOV immediates,
HAL200 load-shader state and a Gen6 CDM direct token. Host tests compare its
bytes to the independently decoded old M4 fixture. Preemption state, metrics,
shader, ESL and output all have their own owned GEM allocations. Output starts
at 0xdeadbeef; success additionally requires 0x44a72000 and the full retirement
checks. `compute-store-1` and `-2` did not write it: both faulted at VA zero,
client context 1, unit 0xd9 (gUPM), raw 0x806a03b311. The client UMA-table view
added in run 2 did not resolve that fault. Firmware +0x2b21c independently
shows SKU binding +0x18 (SKU +0x178) supplies a GPU-read auxiliary address;
zero currently emits register 0x14070=1. The next test allocates that missing
auxiliary backing, with no captured contents or addresses.

`compute-store-3` supplied owned auxiliary backing and cleared the gUPM fault.
It produced nonzero engine timestamps, but firmware repeatedly preempted and
retried the command (KTrace 0x35, reason 5); output stayed 0xdeadbeef and neither
stamp retired. Reason 5 is not yet decoded as a private-memory grow request.
`compute-store-4` populated one fresh 2 MiB private-memory block with generated
4 KiB page entries, a page-list record and matching descriptor counts. The
same retry persisted. Both guarded runs saved evidence and rebooted normally;
peak PMP readings stayed below 53 C. This is GPU activity, not shader success.
The minimal-store body originates in the public M3 decoder fixture; only its
encoding, not that particular M4 hardware execution, was independently checked.

`compute-store-5` added power KTrace and bounded printk independently for power
and engine records. Firmware requests reason-5 preemption on all three engine
classes, including before shader publication; 0x407 carries [1,1], [0,1], [2,1].
`compute-store-6` initialized the named J713 progress, idle, wake, context-switch
and kill timers with producer/common-Asahi defaults. It did not resolve retry.
Both runs saved evidence and rebooted normally. The trace is diagnostic only.

`compute-store-7` rebuilt the diagnostic executable as an M4 code container:
0x340 leading block, two ten-position profile-helper tables, a 0x40-byte CODE
header, a terminating constant program and main entry at +0x3c0. Its state
loader includes the relocated profile helper call and publication suffix from
the completed iadd path. The LDSHDR address scatter agrees with Aurora Mesa;
the old lab subtract-0xaa decoder misleadingly labels +0x3c0 as +0x380.
The generated image/loader host checks pass. Hardware still preempts/retries
with output unchanged; this container correction alone is not execution proof.

## First owned GPU shader result

`compute-esl-fault-1` deliberately addressed the ESL guard page and produced
MMU read fault VA 0x7000024000, context 1, raw 0x8069020211. This established
that the GPU reaches the owned CDM loader mapping. `compute-shader-fault-1`
then displaced only LDSHDR into the shader guard while keeping the helper
address valid; it retained the retry without a shader fault. Both temporary
fault injections were removed and are not present in the committed source.

`compute-store-8` increased the initial fresh private-memory allocation from
2 MiB to 32 MiB (16 generated 2 MiB runs, 8192 pages) with matching descriptor
counts. Metadata still has room to grow; these are startup allocation choices,
not UAPI limits. The exact minimum/occupancy sizing and runtime growth protocol
remain to be derived. This change produced verified output 0x44a72000, both
retirement stamps 0x100, and GPU timestamps 0xff60cc38 / 0xff60ccd9. Queue idle
and 50 ms stability passed, ASC stopped, and the module unloaded successfully.
No captured pool contents, GPU pointers or CPU-computed output were used.

Boot: `1bdcb220-ba9a-45a5-96fc-1ee187fd2b29`. Watchdog: verified 60 seconds without heartbeat.
Six-channel thermal peak: 51 C; no monitor failure.
Evidence: `_provenance/asahi-pivot-20260909/hardware/compute-store-8/`.
This is a kernel qualification shader, not a Vulkan or desktop success.

## Persistent common DRM frontend

`drm-frontend-2` boot 8727f7d6-095b-46cc-b1f0-576c534fa9a7 passed
probe compute qualification, two-client params/GEM/GPUVM allocation and binding,
driver-reserved VA rejection, and platform unbind with both files and CPU
mappings retained. Disconnected ioctls returned ENODEV while CPU GEM data
remained valid. ASC stopped before runtime release and module unload passed.
The first attempt exposed final Rust Drop occurring after devres revocation;
shutdown now runs from platform unbind, with an idempotent Drop fallback.
The shared optional runtime prevents dangling backend pointers and breaks the
DRM/runtime reference cycle at teardown. Failure to establish retirement still
quarantines backing and pins the module until reboot.

Evidence: `_provenance/asahi-pivot-20260909/hardware/drm-frontend-{1,2}/`.
Verified 60-second watchdog, six-channel thermal peak 51.25 C, no sensor failure.
`tools/m4_drm_probe.c` is the bounded destructive-unbind integration test;
run it only inside the watchdog/thermal wrapper. Queue creation and timestamp
mapping are still refused pending their owned userspace submission path.
This does not establish Vulkan or desktop rendering.

## Userspace compute through the shared Asahi UAPI

`user-compute-2`, boot 77e14891-ac11-4d51-97eb-c5c7853f92b9, passed eight
GPU stores from two independent DRM clients. Both mapped their own shader,
ESL, CDM and output at identical user VAs, alternating hardware ASIDs 2/3.
Every syncobj wait returned verified output 0x44a72000 after resetting that
client's output to 0xdeadbeef. Queue reuse and subsequent open-file platform
unbind/module unload passed. Watchdog 60 seconds; thermal peak 51.25 C.
Evidence: `_provenance/asahi-pivot-20260909/hardware/user-compute-{1,2}/`.

The first attempt exposed firmware retaining a job-local notifier after engine
retirement (PC 0x23cd8, FAR at the old threshold). Notification storage now
belongs to the persistent firmware queue and its threshold advances only when
DRM schedules runnable work. Command allocations remain private to each job.

DRM entities share an initial one-credit device scheduler. Submission returns
before execution completes; a workqueue polls exact engine/firmware retirement
and signals the hardware fence. Accepted packets retain the common VM job guard,
and unproven retirement quarantines DMA backing until reboot. Timestamps map
through common GEM and convert the 24 MHz clock to UAPI nanoseconds, but user
timestamp delivery and scheduler dependency combinations are not yet tested.

Render, sampler/helper state and nonzero compute flags are explicitly refused.
This is a direct UAPI diagnostic using a generated known shader, not Vulkan.
The current per-command 32 MiB private pool is conservative allocation policy;
its reuse, adaptive sizing and growth remain work. Ring wrap is implemented but
the hardware test covered eight user entries, not a complete wrap.

## Typed M4 render wire translation (host validation)

The render envelope, firmware sequence, parameter manager and one-layer register
producers now accept explicit owner-supplied addresses. TA and fragment have
separate layouts; partial-render program and depth/stencil mirrors are retained.
No old runtime, global capture graph or lab submission code is linked by these
encoders. Samplers, helper programs, compression and MSAA need further porting.

The `tools/m4_render_{wire,state}_{fixture.rs,reference.c}` differential fixtures
compare complete byte streams against the previously executed M4 C encoders.
Both comparisons pass, including full-panel and odd-size geometry, 16/32-pixel
utiles, relocated addresses, nonzero resource high words and depth/stencil state.
This establishes encoding parity, not new-driver hardware rendering.

## Owned render submission

The M4 DRM scheduler can now translate the one-layer, one-sample render UAPI
into owned TA and fragment commands. Each pass owns its parameter memory,
geometry-sized tilemap/TPC, USC pool, context-save storage, command state and
firmware aliases. InitPB and TA publish atomically as a batch; fragment follows
exact TA retirement. Failed work retains all DMA backing until reboot.

The initial one-credit scheduler uses one parameter slot. Its last retired
owner stays alive until a replacement pass retires, as firmware caches this
slot. Shared hardware tables and the architectural M4 compact context-save
range are reserved in each common GPUVM. Mesa does not own that scratch page.
Render timestamps, sampler/helper state, compressed Z/S and multisampling are
explicitly rejected until their wire paths are ported. This is not yet a claim
of Vulkan rendering or desktop parity.

The render-table/mapping prerequisites preserved the real Vulkan compute
result in `render-prereqs-compute-1`: 32 expected outputs, safe open-file
teardown, maximum observed PMP temperature 51.75 C, watchdog verified at 60 s.

### First verified Vulkan graphics result

`vulkan-render-3` rendered a 32x32 RGBA8 triangle through real G16G Honeykrisp
and this common DRM path. Both TA and fragment retired with both stamps 0x100;
the Vulkan test verified every pixel (checksum `a513efed37c25325`). Readback
used VK_EXT_host_image_copy to isolate rendering from the later GPU copy test.
Open-file unbind and module unload passed. Maximum observed PMP temperature
was 53.75 C; watchdog was verified at 60 s with no unconditional heartbeat.

Before testing repeated renders, bootstrap command VAs were separated from
client backing within the reserved driver arena. Otherwise a retained previous
pass could occupy the next command VA selected independently in context zero.
This follows the lifetime requirement for firmware-cached parameter managers;
it does not change any client VA or render-size limit.

Repeated submission is not yet qualified: `vulkan-render-repeat-1` entered
firmware recovery after the first completed pass; repeat-2 identified reason 3
(translation fault), this time after TA on the first pass. No unproven backing
was freed, and the guard saved evidence and rebooted. Recovery diagnostics now
snapshot owned firmware RAM and the powered engine's already-selected fault
bank without selecting or acknowledging hardware banks. Do not infer a fault
address from the all-zero statistics payload alone.

### Repeated render qualification and USC freelist capacity

The selected fault bank in `vulkan-render-repeat-3` identified a gUPM write
40h past the 64 KiB freelist. Its descriptor advertised 8192 entries, all
populated. The render allocator now leaves capacity beyond the populated
entries, as the qualified compute allocator already does: 16384 qwords for
8192 pages. The shared encoder rejects a completely full freelist.

`vulkan-render-repeat-4` retired eight render passes across two client VMs,
including four interleaved compute copies, without firmware recovery. Four
host-copy renders verified every pixel, checksum `a513efed37c25325`. The GPU
image-to-buffer copy completed but returned an incorrect green channel; this
is not yet graphics/desktop parity. Peak observed PMP temperature was 53.5 C
with the verified 60-second watchdog and thermal guard.

### Desktop qualification and idle firmware notifications

`hyprland-native-5` passes the full shared-buffer Zink compositing check and
starts the actual Omarchy Hyprland configuration on the 2560x1664 panel. The
screencopy and imported native DCP scanout each contain 3,922,542 expected
red test-terminal pixels. The panel/background then appear with SuperTux;
its active OpenGL context identifies M4 Zink/Honeykrisp. The game menu was
visually verified in native-3; native-5 screenshots show the level intro,
not yet gameplay. Peak observed temperature 53.5 C, watchdog verified 60 s.

RTKit event notifications now enqueue the existing serialized completion
worker, which drains firmware channels even when no userspace job is active.
Previously notifications after the last job were only logged; firmware
retried them and overwhelmed the kernel log. The callback never takes the
runtime mutex. Its DRM reference is disconnected by the existing unbind
ownership path. Idle firmware errors quarantine all backing until reboot.
The native-5 run builds and tests this notification path; a continuous kernel
log preserves completion evidence independently of ring-buffer wrap.

`hyprland-live-1` subsequently reaches correct visible SuperTux gameplay in
Welcome to Antarctica on the actual Omarchy desktop, using this GPU driver
and Mesa Zink/Honeykrisp. The active game context confirms Apple M4 G16G B0.
The observed floating-window HUD is 32/33/34 FPS min/average/max; this is not
a sustained benchmark or a 60 FPS claim. The intro is input-driven, and a
held space press after setup enters gameplay.

The sustained guard has passed 48 consecutive fresh GPU/compositor checks
at the evidence snapshot, with DCP reporting 1405 completed imported swaps
and zero failures. All 32 compute output values are verified on each check.
Peak observed PMP temperature was 54.5 C, with no sensor failure, CPU policy
preserved and GPU at the 338 MHz boot state. The session is bounded to 30
minutes and renews the 60-second watchdog only on verified progress with a
live thermal monitor. Mesa's `tests/m4-desktop` contains the qualification
launcher, guard and read-only context logger. Full firmware recovery, GPU
thermal throttling and broader Vulkan conformance remain future work.

### SuperTuxKart parameter-buffer pressure

`stk-demo-17` reproduces a first-scene tiling stall after 8,274 retired jobs.
The M4 firmware emits FinishTA followed by trace `0x12`, but neither final
tiling stamp is written. There is no selected-bank MMU fault. The previous
runtime only published fragments after final TA retirement, so a pass needing
a partial fragment to reclaim parameter pages could not make progress.

The runtime now publishes InitPB/TA and a dependent fragment together. The
M4 type-4 barrier names the TA event slot and target stamp; firmware owns the
stage dependency and partial-render scheduling. Both command graphs, their
GPUVM binding and shared heap remain retained until both stages retire.
The initial heap remains 4 MiB and the stage timeout remains two seconds.

The barrier layout is cross-checked against the local matching 25G83 producer
`AGXChannel::setupBarrierCommand` at `fffffe000896b734` and firmware consumer
`agx_drain_work_queue_commands` at `0x1cd20`. These are dirtyroom references;
no executable firmware payload is modified or included by this change.
Five standalone host checks pass, including the exact dependency record and
rejection before output mutation for invalid fields.

`parameter-pressure-1` qualifies this change on J713, boot
`838b6e1f-206e-4134-af5a-b05c4681748c`. Four ordinary attribute/uniform draws
and four pressure draws each verify 4,096 exact pixels through Zink/Honeykrisp.
Each pressure draw submits 65,536 six-vertex instances. Firmware trace `0x12`
records both the pause and resume, followed by final TA and fragment stamps.
Peak observed PMP temperature is 52 C; the 60-second watchdog and six-sensor
thermal guard cover the test. The full SuperTuxKart race is still pending.

## Retain the compute UMA owner across context switches

0 A.D. release 28 exposed a private-memory lifetime bug during its first
196608-byte index-buffer update. In `0ad-map-7` the current compute pool was
`0x7102098000`, but the latched GPU fault addressed `0x7104924b00`, inside the
preceding compositor compute pool at `0x7104920000`. The hardware UMA slot
still encoded that preceding pool. Reusing the canonical command/UMA
address after completion allowed firmware's cached descriptor to outlive
its backing when the client ASID changed.

Retain the completed compute owner until a distinct replacement retires,
mirroring the existing render parameter-manager lifetime. Box the compute
work/retired owner to avoid growing the runtime object on the kernel stack.
The first by-value retention experiment reset during startup twice; no
persisted panic established the exact reset cause. The boxed version booted
and passed the previously failing upload alongside Hyprland (`0ad-map-10`).
The game then reached a separate compiler refusal for an FMA with absolute
value and immediate operands. No GPU device loss occurred in that run.
Peak monitored PMP temperature was 53.75 C; the 60-second watchdog and all
thermal checks remained mandatory. Full kernel modules built successfully.
Evidence: workspace `_provenance/asahi-0ad-20260909/`.

### Render event values during 0 A.D. bring-up (2026-09-10)

The owned M4 runtime now advances render event values by 0x100, as common
Asahi queues do, and polls the value carried by the active render. Both TA
and fragment metadata, InitPB and their dependency use that value. Values
are shared across user contexts for the retained hardware queues; the wire
encoder's reserved zero value is skipped on wrap.

M4/25G83 firmware `agx_drain_work_queue_commands` at 0x1cf74 and
0x1cfc8 extracts stamp bits 14:8 to select internal command records. A fixed
0x100 aliases successive renders to the same record. This patch removes
that aliasing; it does not establish the cause of all observed stalls.
`0ad-scene-render-stamp` retired the upload passes with increasing values,
then still stalled on the large first scene. No complete 0 A.D. frame was
qualified by that run. The five standalone render dependency/geometry
checks and the ARM64 module build passed. Watchdog 60 s, six PMP channels,
CPU thermal policy and the unchanged 338 MHz GPU clock remain mandatory.

Compute now carries its own advancing event value after the bootstrap
0x100 event; dispatch metadata, finish sequence and retirement checks agree
on it. The four standalone compute wire tests and ARM64 module build pass.
`0ad-fma-new-stamp-cold` still returned unchanged values, so event aliasing
is not established as the cause of that cold-start failure. A subsequent
synchronized scene replay completed without a kernel fault but rendered
mostly black; it is not a passing scene. Neither event-value change raises
clocks or relaxes retirement/watchdog checks.

### Retained CPU mapping borrows (2026-09-10)

M4 firmware buffers now borrow their ObjectRef's retained VMap for CPU
accesses. Previously every word in the 8,192-entry private-page freelist
created and dropped a second VMapRef, taking the GEM reservation lock on
both operations. The owner already pins the mapping and backing. Borrowing
it directly avoids those extra locks and reference-count operations; WC
attributes, volatile shared-state accesses, initialization and device
barriers are unchanged. No Mesa change is needed.

The ARM64 module build passes (60 existing warnings). On J713,
`0ad-kernel-map-baseline` and `0ad-kernel-map-cached` replay the same
synchronized prefix through call 23123 with the same installed Mesa. Both
retire 1,360 jobs, including 1,351 renders, and produce pixel-identical PNGs.
First-to-last retirement spans are 29.185 s and 28.360 s respectively; the
summed previous-retirement-to-render-construction gaps decrease from
14.172 s to 13.176 s. These include host scheduling and tracing overhead:
they are a single replay comparison, not pure GPU timings or a game FPS
claim. Peak PMP readings are 52.25 C and 53 C.

The first cold pixel check (`0ad-cached-vmap-pressure`) reproduces the
already observed all-zero readback without a GPU fault. After the replay,
`0ad-cached-vmap-pressure-warm` passes both four-draw attribute/uniform and
four-draw parameter-pressure checks, each checking 4,096 exact pixels.
The cold-readback issue, batched-scene hang and building artifacts remain
unresolved. All runs arm/verify the 60-second watchdog and monitor all six
PMP channels; CPU thermal policy and the 338 MHz GPU state are preserved.
Evidence remains in workspace `_provenance/asahi-0ad-20260909/`.

### Recycle render backing after replacement retirement (2026-09-10)

The serialized runtime keeps one reusable render owner, separately from
its most recently retired owner. A render only becomes reusable after a
distinct replacement has retired both TA and fragment stages. The next
render may take that spare only when VmBind identity and width, height and
utile dimensions match. A miss allocates normally; this imposes no draw,
framebuffer or submission limit. Render owners are boxed to avoid adding a
large by-value object to the runtime and worker stacks.

Recycling retains GEM allocations, CPU/firmware/GPU mappings and the ASID
lease. Every buffer is zeroed, including private memory and parameter-buffer
contents, then command records, stamps, freelists and dependencies are
rebuilt. The most recent firmware owner remains untouched. Publication,
completion, fault quarantine and CPU/GPU memory ordering are unchanged.

The ARM64 module build passes. `0ad-kernel-render-reuse` replays the same
prefix through call 23123 used above, retiring 1,360 jobs and 1,351 renders
with exactly the baseline pixels. Of those renders, 858 reuse backing and
493 allocate. The trace confirms every reused owner follows a distinct
fully retired render. First-to-last retirement takes 20.387 s versus
29.185 s before both kernel optimizations, about 30% less time. The summed
setup/scheduling gaps fall to 7.584 s and their median to 2.118 ms; these
remain traced replay measurements, not game FPS or pure GPU timings.
Peak PMP temperature is 53.75 C. `0ad-render-reuse-pressure` also passes
both the four ordinary draws and the four 65,536-instance pressure draws,
checking 4,096 exact pixels in each mode.

`0ad-kernel-reuse-desktop` then passes the bounded 180-second 0 A.D.
Acropolis Bay session on the physical 2560x1664 Hyprland desktop: 30 fresh
Vulkan/compositor checks, 388 thermal samples, peak 55 C, no GPU fault or
thermal trip. The final screenshot shows the map and the known incorrect
building surfaces; the overlay still reads 1 FPS. The replay speedup does
not resolve those artifacts, cold readback or the need for synchronized
draws. Mesa remains unchanged throughout this kernel comparison. Evidence
is saved before the guard stops the session and disarms the verified
60-second watchdog; CPU thermal policy and the 338 MHz GPU state are kept.


### Forwarding job memory attachments (2026-09-10)

The M4 UAPI adapter validated vertex, fragment and compute attachment lists
but discarded them before constructing firmware work. Commands now retain
the lists and encode the 25G83 start records, counts and finalize flags for
each engine. Byte ranges are normalized to 128-byte lines and the entire
normalized writable mapping is validated before publication. Overflow and
the firmware's sixteen-record capacity are checked. The writable descriptor
attributes 0x18/3 match the retained M4 Ocean witness; this does not enable
compressed resources. No mappings are synthesized from capture addresses.

Standalone encoding tests cover both edge lines, overflow, capacity and the
native descriptor fixture. The ARM64 module builds. Firmware event 0x116
confirms receipt of the exact destinations, and the 128x128 mode0/mode2
control passes both readbacks. The 1024x1024 loop20 workload still times out,
and cold Vulkan fill still has stale first-read data; attachments are not
a qualified fix for either defect or for full-scene 0 A.D. rendering.
Experiments retain the 60-second watchdog, PMP monitoring and boot pstate.

### Compute firmware pointer identities

StartCompute stats (+1c), FinalizeCompute stats (+4), and the Runtime+264
engine state now reference the same retained Config object. The work_queue
pointers in start, timestamps, and finish reference the actual QueueInfo.
The old lab wiring incorrectly substituted a notifier threshold for stats and
a zero scratch buffer for QueueInfo. Exact 25G83 iadd captures establish both
identities; the common Asahi compute builder uses these same object roles.

Five standalone compute wire tests and the ARM64 module build pass. Guarded
cold fill 20260910T183822 still had 106644 stale words at its fence and zero
on the later read; this corrects object wiring but does not qualify a complete
memory-visibility fix. Firmware and the boot qualification shader ran.

Render statistics now similarly use Runtime+254 backing plus four bytes for
vertex and Runtime+25c backing plus eight for fragment, replacing per-job
scratch. Exact 25G83 initFirmwareSharedData (89a8388..89a840c) and native
TA/3D SKU producers (89cb7e0, 89bf61c) establish these shared object identities
and header offsets. Start/finalize fixture assertions and ARM64 build pass.
Guarded 184155 1024-pixel-square mipmap loop20 still times out: the object-role
fix alone does not resolve the large fragment failure.

The context-control flag in StartVertex+48 and StartFragment+60 is enabled
as in native/common Asahi construction; compute+30 is fixed to one instead
of being confused with RunCompute's separate ASID-generation byte. Firmware
uses this flag when updating the context mask at SGX+d8f0. Added bounded
read-only global engine-state snapshots on faults. Wire tests and ARM64 build
pass. Guarded 184607 large loop still times out; cold small 184910 has stale
first read then correct later reads, while warm 185026 passes both modes and
all reads. These changes do not establish a visibility or timeout cure.
