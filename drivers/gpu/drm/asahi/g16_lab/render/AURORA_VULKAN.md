# Vulkan and Zink integration direction, 2026-09-07

V1 targets Honeykrisp Vulkan with Zink providing the OpenGL translation path.
Native Asahi Gallium/OpenGL development is outside this workstream. The existing
J713 lab renderer remains the hardware regression test during integration.

## Pinned reference sources

Workspace-relative checkouts, independent of this kernel's Git metadata:

| Checkout | Branch | Revision |
| --- | --- | --- |
| `linux/aurora-gpu` | `drm-agx3` | `4ac2664258f37` |
| `mesa/aurora` | `agx3` | `e58d46899eb1` |

Origins are `https://github.com/aurora-silicon/linux-aurora.git` and
`https://github.com/aurora-silicon/mesa.git`. The kernel reference advanced
12 commits from `48793bece781f`; this working kernel was not merged with it.
The refresh manifest and offline cross-check live in workspace
`_provenance/aurora-vulkan-refresh-20260907/`.

## What the fetched source gives us

Mesa `src/asahi/lib/agx_chip.c` maps G16G and G17P to AGX3b, HAL200 command
streams, and generation-9 descriptors. These shared classifications are a
reuse starting point, not evidence that every state field is interchangeable.

* `compiler/agx3_compose/`: field-based AGX3b/AGX3c instruction encoders.
* `compiler/agx3_vary_core.c`: UVSW vertex exports, raw coefficient loads,
  and fragment interpolation, including separately identified G17P markers.
* `isa/AGX3b.xml`: an available decoder, superseding the need to reconstruct
  one from the local Apple LLVM library just to inspect M4 instructions.
* `compiler/agx3_pack_binary.c`: lowering and scheduling, including complete
  position-export and user-varying protocols.
* `genxml/agx3_cmdstream.h`, `vulkan/hk_agx3_*`, and native sampler code:
  command/state builders to compare with the qualified M4 lab state.
* `vulkan/tests/`: offscreen and WSI workloads for progressing from readback
  to presentation once the M4 userspace/kernel contract is implemented.

The newest kernel changes concern render intervals, queue contexts, UAT mapping
ownership, retained FList state, global-requestor visibility, and render VM
handoff. These are useful lifecycle references; their G17P firmware structures
must be checked against our J713/25G83 evidence before integration.

## Checks performed locally

Built the existing standalone compose-corpus and varying-core tests using C11,
`-Wall -Wextra -Werror` and AddressSanitizer/UndefinedBehaviorSanitizer.
All 1,494 corpus cases and all 977 varying checks passed. These are offline
encoder checks, not new hardware execution.

A separate check calls `agx3_vary_uvsw(AGX_ISA_ENCODING_AGX3B, c * 2, c,
c + 1, true, 1u << c, ...)` for components 0 through 3. All four instructions
match the J713-tested position exports in `tools/g16graphics.py` byte-for-byte.
This establishes a concrete shared encoding for our current vertex output.
It does not establish M4 interpolation scheduling or coefficient setup.

## Next implementation gates

`tools/g16agx3.py` now builds a host encoder library from the pinned Mesa
checkout. `tools/g16graphics.py --agx3-library ... --header ...` uses it for
the four position exports. Regenerating the existing graphics header is
byte-exact, including all unchanged shader bodies. No kernel rebuild or
hardware experiment is required to establish that identical-byte check.

From the workspace root:

```sh
python3 linux/linux-alpha/drivers/gpu/drm/asahi/g16_lab/tools/g16agx3.py \
  --mesa mesa/aurora --output build/aurora-agx3-offline/libg16agx3.so
python3 linux/linux-alpha/drivers/gpu/drm/asahi/g16_lab/tools/g16graphics.py \
  --agx3-library build/aurora-agx3-offline/libg16agx3.so --header /tmp/graphics.h
```

1. Use the AGX3b encoder and decoder for the lab's vertex exports and fragment
   varying instructions. Keep the eight-frame generated-stream test green.
2. Implement M4 coefficient/interpolation setup. Start with one linear varying,
   then RGB, and compare the entire rendered target against a CPU reference.
   Change vertex attributes while holding geometry fixed to distinguish real
   varying interpolation from the current fragment-coordinate arithmetic.
3. Connect an explicit T8132/G16G kernel/userspace contract to Honeykrisp.
   Preserve M4 firmware tuning, queue ownership, mappings, and retirement.
   Exercise public Vulkan compute and offscreen graphics before WSI.
4. Validate textures, render-target reload/store, synchronization and buffer
   sharing needed for presentation. Then test WSI and Zink requirements.

At this revision `agx_chip_driver_supported()` admits only G17P among AGX3
devices, and `agx_params_driver_supported()` requires its exact T8140 contract.
M4 needs its own implementation and feature admission; relabeling it as G17P
would bypass meaningful platform checks. Neo desktop results do not qualify
M4 desktop rendering. No GPU experiments were run for this source refresh.

## M4 interpolation implementation, 2026-09-08

Steps 1 and 2 now pass on J713/25G83: Aurora-encoded UVSW/COPY/interpolation,
scalar linear color and three-component RGB, including eight alternating
frames and exact full-target comparisons. The separate PPP varying-count
record was essential; VIT and output sizes alone yielded zero user coefficients.
See `VARYING_BRINGUP.md` and its retained validation manifests. The original
eight-frame generated-stream regression also passes. Captured helper state
and firmware setup remain in use; public Vulkan integration is still pending.

The next compiler bridge also passes: `tools/g16mesa.c` sends the bounded RGB
workload through Mesa NIR optimization, register allocation and AGX3b codegen.
Both compiled stages pass eight alternating frames on J713 (524,288 exact
pixels). See `MESA_NIR.md` for reproduction and the separate broader-suite
failures. The host Honeykrisp/Zink build succeeds. This does not yet connect
SPIR-V/Vulkan submission or presentation to an M4 DRM device.

## DRM memory groundwork, 2026-09-08

The root-only `stage=drm-frontend` creates a temporary `g16-lab` DRM device
without starting GPU firmware. GEM_CREATE / MMAP_OFFSET and VM_CREATE /
VM_DESTROY / VM_BIND use the pinned Asahi wire ABI, checked against its header
by `tools/g16drmtest.c`. The driver deliberately retains its lab identity and
has no GPU submission ioctl or advertised Asahi capabilities yet.

The VM implementation builds private, unpublished 42-bit / 16 KiB UAT roots
from pinned SHMEM pages. It verifies every physical leaf, retains a GEM
reference and vmap pin for each binding, and rebuilds before replacing a root
so allocation errors preserve existing bindings. This is not a live GPU VM:
there is no hardware context publication or TLB invalidation in this stage.
Mapping permissions still require GPU validation before publishing these roots.

The bounded interface currently allows one VM per file, one bind per ioctl,
exact-range unbind, 128 mappings / 256 MiB of mapped ranges, and cumulative
64-object / 256 MiB GEM allocation per open. VM_PRIVATE, single-page mappings,
PRIME, synchronization, queues and submission are not implemented.

On J713/25G83, `tools/run-drm-memory.py` armed the 60-second watchdog before
loading the module. The test verified zeroed WC/writeback allocations, CPU
mapping aliases and 131,072 bytes of contents after GEM handle close, foreign
handle/mmap rejection, allocation bounds, private VM binding and aliasing,
reserved-range/overlap/size/alignment validation, mapping lifetime after handle
close, VM destruction and file-close teardown, and stale VM ID rejection.
The module unloaded, its render node disappeared, and no new kernel fault was
logged. Evidence was saved before watchdog disarm. See
`validation/j713-drm-vm-20260908.json`. No GPU firmware started in this test;
public Vulkan drawing and window presentation remain outstanding.


## Hyprland focus

The current implementation and validation target is Hyprland GLES through
Zink/Honeykrisp. `HYPRLAND.md` records the installed compositor's source
requirements, GLES 3.0 fallback, dependency order, and the first successful
GPU-backed GEM/DMA-BUF target and completion-fence test on J713.
