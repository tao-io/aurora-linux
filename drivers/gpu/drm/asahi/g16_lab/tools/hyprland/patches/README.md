# Native compositor scheduling fix

Apply the included patch to Hyprland 0.56.1 (upstream5c9377c). The newer
native-fence scheduler can queue its presentation callback even when no new
buffer is pending. Its unconditional commit becomes a blocking KMS update
without a page-flip event, freezing the input loop while it waits for GPU
fences and the display. Check the pending-buffer bit in the deferred callback;
normal frame commits, modesets and fence waits are unchanged.

J713 trace evidence: the unpatched compositor's ~80ms mouse IPC stalls spent
time in drm_atomic_helper_wait_for_fences, drm_atomic_helper_swap_state and the
native DCP swap. The captured atomic request had flags0 and reprogrammed the
existing primary framebuffer. After the patch, a22s main-thread sample caught
none of those blocking atomic waits. Menu-opening cursor IPC was below7ms;
one46.8ms outlier coincided with the test harness's scheduled screencopy and
waited on a render syncobj. Display presentation still had50–83ms gaps during
menu opening; this fixes input-loop blockage, not every GPU latency source.

Guarded run:20260911T221823823332Z-quickshell-no-empty-commit-cold. Initial red
scanout/screencopy and actual M4 Zink/Vulkan renderer checks passed. A preceding
warm restart (221635) faulted during startup; its cause remains unestablished.
The passing run began after the watchdog-supervised reboot.

ARM64 binary SHA-256:
63358413d2a1c518c23594896e59a6613493a2f0952af0ccec9c33a353b2c2c1

The collection's tools/build-hyprland.sh builds incrementally with its existing
Omarchy sysroot and pinned upstream dependencies. The binary is installed at
/opt/m4-hyprland/bin/Hyprland and selected with start-hyprland --path; the system
binary remains available. desktop-experiment.py --hyprland-binary records the
selected binary hash and source patch. Inspect final guard results separately
from these short latency measurements.


Final qualification: the cold run completed79 health checks and815 thermal
samples, peak55.75C, with no GPU/DCP failure. The final menu screenshot was
inspected and rendered correctly. Six-second steady-motion intervals:
desktop59.80FPS,max16.699ms; menu54.09FPS,max33.498ms, no interval above50ms.
Cursor IPC maxima were6.47ms and7.21ms. The user reported "way better" with
small dips remaining. This is not a claim of a continuously locked60FPS menu.
