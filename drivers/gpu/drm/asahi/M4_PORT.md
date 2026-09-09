# M4 integration into Asahi

The completion target is parity with the old driver: correctly rendered,
GPU-accelerated Hyprland and SuperTux through Asahi and Mesa Vulkan/Zink.
A first verified Vulkan program is an intermediate milestone, not completion.

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
`CONFIG_DRM_ASAHI` build. M4 execution remains disabled in the imported driver.

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
