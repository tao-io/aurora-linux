# M4 integration into Asahi

The `m4-asahi-port` branch imports the complete Asahi driver and UAPI from
Aurora `drm-agx3`, commit `4ac2664258f370cf7b03327da742b5885f91ca0b`.
The unmodified import is `ebac9112f313`; transport dependency integration is
`60e61fd5fdf7`. RTKit retains the existing CoreAnalytics support and initializes
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

## Runtime integration boundary

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

The pure M4 encoder tests require no kernel or GPU:

```
rustc --edition 2021 --test drivers/gpu/drm/asahi/g16_render.rs -o /tmp/g16-render-tests
/tmp/g16-render-tests
```

Hardware experiments must keep the workspace's verified 60-second watchdog,
all six PMP temperature channels, enabled CPU thermal policy and conservative
test clock caps. Do not bind Asahi while the lab owns the GPU. No new runtime
has been loaded or booted as part of this initial source integration.
