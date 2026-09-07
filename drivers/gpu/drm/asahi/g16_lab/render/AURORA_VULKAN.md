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
