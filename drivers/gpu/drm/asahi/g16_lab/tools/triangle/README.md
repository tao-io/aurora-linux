# Vulkan triangle application — M4 driver connection pending

This ordinary Vulkan/XCB application creates a 256x256 window, uploads a
48-byte position/color vertex buffer, compiles the accompanying SPIR-V into a
graphics pipeline, and submits draws to a swapchain. It uses Vulkan 1.1,
RGBA8/BGRA8 UNORM, one queue, one color attachment, no descriptors, depth or
multisampling. The close button exits. Resizing is not implemented: an
out-of-date swapchain ends the bounded test. The default is 600 frames.

The source is adapted from Aurora Mesa `e58d46899eb1`'s
`src/asahi/vulkan/tests/g17p_wsi_triangle.c`, retaining its MIT copyright.
Changes include the tested RGB shader inputs, window close handling,
serialized presentation/semaphore reuse, M4 device selection, and explicit
rejection of CPU Vulkan devices. It uses the system Vulkan loader; there is
no replacement ICD or Vulkan-to-lab-call shim.

Build with a C compiler, Vulkan/XCB headers, glslang and SPIRV-Tools:

```sh
python3 build.py --output /tmp/m4-vulkan-triangle
/tmp/m4-vulkan-triangle/m4-vulkan-triangle 600
```

The default device ID check is T8132 (`0x8132`). `--development-gpu` allows a
different hardware GPU for application testing; it still rejects CPU devices.
This option does not establish M4 support.

## Actual validation, 2026-09-08

* The app and SPIR-V build on the x86 development host; the app also builds on
  the ARM64 Omarchy machine with `-Wall -Wextra -Werror`.
* The application draws the RGB triangle and completes on the development
  host's AMD Radeon 780M using RADV. A screenshot was inspected. The Xvfb test
  uses `MESA_VK_WSI_DEBUG=sw` for CPU-copy **presentation**; Vulkan rendering
  remains on the selected AMD hardware. This is an application test, not M4
  hardware validation.
* These same GLSL/SPIR-V shaders compile through Mesa's AGX3b backend and pass
  eight offscreen M4 lab frames (524,288 exact pixels). See
  `../../render/validation/j713-spirv-rgb-20260908.json`. That renderer does not
  consume Vulkan API submissions yet.
* Running this application on Omarchy under the verified 60-second watchdog
  fails at `vkEnumeratePhysicalDevices` with `VK_ERROR_INITIALIZATION_FAILED`
  (-3). There is no GPU render node after lab teardown. The evidence is in
  workspace `_provenance/omarchy-m4-render/hardware/vulkan-window-20260908/`.
  The watchdog was disarmed after saving the result.

**A Vulkan triangle has not yet been presented on M4.** The next required
connection is the Asahi DRM queue/submit/synchronization contract backed by
M4 firmware, followed by M4 admission in Honeykrisp and its WSI buffer path.
The current root-only DRM frontend provides GEM and private VM memory tests;
it does not advertise or implement GPU execution.
