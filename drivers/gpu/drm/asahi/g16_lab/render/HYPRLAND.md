# M4 Hyprland acceleration target

The target is Hyprland's GLES renderer through Mesa Zink and Honeykrisp Vulkan
on J713/T8132/G16G. A working triangle alone does not qualify the compositor.
The current desktop remains on its existing software-rendered path.

## Reference matching the installed compositor

Hyprland 0.56.1, commit `5c9377c15f85c50648f35ca5a213754f95b93ca0`, is installed
on Omarchy. The matching unmodified source is in workspace
`reference/hyprland-0.56.1`, cloned from `https://github.com/hyprwm/Hyprland.git`.
The relevant source contracts are:

* `src/render/OpenGL.cpp:199`: request GLES 3.2, retry GLES 3.0 on failure.
  GLES 3.0 is therefore a valid first compositor target. Do not fake API
  versions or advertise unsupported Vulkan features to reach it.
* `OpenGL.cpp:294`: EGL images, DMA-BUF formats/modifiers and native fences.
  `eglCreateSyncKHR`, `eglDupNativeFenceFDANDROID` and `eglWaitSyncKHR` are
  explicitly required by this version.
* `OpenGL.cpp:984`: premultiplied alpha, `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`.
* `shaders/glsl/surface.frag`: sampled 2D window textures, optional alpha,
  tint/discard/rounding, blur, color-management and other variants.
* `GLRenderer.cpp:127`: buffer-release synchronization and monitor fences.

For a first nested compositor validation, use a separate minimal configuration
without blur, shadows, rounding, color-management or animation effects, then
add them incrementally. No running user configuration was changed for this
work. Keep the production session available while validating the new renderer.

## Dependency order

1. GPU-visible GEM memory, exported image lifetime and completion fences.
2. Asahi queue/submit/syncobj contract backed by the M4 firmware worker, with
   client-owned VM mappings and actual uploaded command streams/shaders.
3. Explicit M4 Honeykrisp support: offscreen Vulkan rendering, textures,
   sampler states, premultiplied blending, render-target load/store and reuse.
4. A Mesa desktop build with Zink, EGL, GLES and GBM enabled. The original
   compiler-only host build has EGL/GLES/GBM disabled and is insufficient.
5. GLES textured-surface compositing through Zink, EGL image sharing/native
   fences and display-buffer handoff, then a nested Hyprland test.
6. Qualify the real desktop, longer lifetimes, recovery and optional effects.

The GPU can render while the existing compositor presents through a CPU copy.
That is useful for bring-up, but the endpoint here is acceleration of Hyprland
itself, including composing its window textures. Preserve the distinction in
results. The current simple-framebuffer display path is also separate from
GPU rendering and needs a compatible presentation route.

## GPU-backed exported target milestone, 2026-09-08

`tools/run-render.py --render-drm-target` starts the existing bounded eight-frame
worker and the root-only `g16-lab` DRM node. Its target is a SHMEM GEM object;
the GPU UAT maps the actual GEM pages. `G16_LAB_TARGET` is an explicitly lab-only
accessor for its handle/size/layout, not an Asahi submission implementation.
PRIME exports the target as a DMA-BUF. Command and firmware buffers remain
private. The image is uncompressed RGBA8 in 64x64 Morton tiles with a 16 KiB
prefix, not a linear scanout image or a qualified compositor modifier.

Each draw has a write fence in the object's reservation. The first pending
fence exists before exporting; successful work signals after GPU retirement
and pixel verification. Failure/cancellation defers signalling until the
firmware worker has returned from teardown. Objects and UAT mappings stay
alive for the complete worker lifetime; the target is released before the DRM
device. PRIME's core exporter pins the module while DMA-BUF references exist.

`tools/g16targettest.c` exports the target, closes the DRM handle and file,
then closes the parent's DMA-BUF fd. A separate UID-1000 child owns the remaining
exported fd, verifies the initial pending DMA-BUF/sync-file fences, waits for
each GPU completion, uses DMA_BUF_IOCTL_SYNC for CPU access, and checks every
pixel. A parent/child acknowledgement prevents target reuse during CPU reads.
This bounded probe does not yet implement arbitrary reader dependencies,
client submission, cross-device DMA mapping, EGL import or a swapchain.

J713 run `20260907T231120Z-render-ta` passed eight frames / 524,288 exact pixels,
including the unprivileged reader, pending-to-complete fence transition,
exported sync-file completion, and CPU access synchronization. Firmware KTrace
and module teardown passed. The 60-second watchdog was armed before loading
and disarmed after evidence collection. See
`validation/j713-drm-target-20260908.json`; complete evidence is in workspace
`_provenance/omarchy-m4-render/hardware/drm-target-20260908/`.

This is working M4 GPU output in a shareable, fenced DRM buffer. It is not yet
public Vulkan submission, Zink acceleration or accelerated Hyprland.

## Desktop build and compositing test

The isolated native build profile in `tools/hyprland/build.py` enables
EGL/GLES/GBM, X11/Wayland, Zink and Honeykrisp. It succeeds on the x86 development
host at Aurora Mesa `e58d46899eb1` without changing system libraries. The local
`mesa/m4-hyprland` branch is a separate working checkout; the reference stays
unchanged. An ARM64 Mesa build and M4 implementation are still required.

`tools/hyprland/composite.c` now supplies a GLES 3.0 readiness test: nearest
and linear sampling, premultiplied alpha, scissor, texture updates, EGLImage
DMA-BUF export/import and retained texture lifetime, native fence export,
import and server waits, and complete readback verification. Private Zink on
AMD RADV passes all eight frames / 8,192 pixels with at most one channel unit
of error. This is a checked test workload and a development-GPU result, not
M4 graphics qualification. Its runner enforces the J713 watchdog and preserves
results before disarming. See `tools/hyprland/README.md` and
`validation/hyprland-zink-development-20260908.json`.

## Native Zink compositing, 2026-09-08

`20260908T004735Z-render-ta` passes the local-texture variant, and
`20260908T060443Z-render-ta` passes the complete shared-image gate: eight frames,
8,192 checked pixels, nearest/linear sampling, premultiplied blending, scissor,
texture updates, EGL DMA-BUF export/import and eight native fence import waits.
The latter executes 16 GPU render passes and cleanly unloads after saving traces.
Client mode now permits 512 live GEM objects and 1,024 VM bindings while keeping
the existing 256 MiB byte limits. This replaces the initial 64-object ceiling
that stopped the shared-image test during pipeline creation.

These diagnostic runs use a process-local `MESA_GLES_VERSION_OVERRIDE=3.0`:
native transform feedback and some optional features are still disabled.
Unlike a full GLES readiness qualification, the reported result covers the
pixel-checked compositor subset only. The GPU draws real Vulkan shader work;
Vulkan host-image-copy performs CPU upload/readback after synchronization.
Hyprland itself and the physical display presentation path remain to be tested.
