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
