# Hyprland / Zink readiness gate

## Current supervised Omarchy session

J713/T8132 now runs the installed user's Omarchy configuration through private
Zink/Honeykrisp libraries. `start-omarchy.sh e 90` passed the physical 2560x1600
screen gate and 18 independently checked Vulkan health probes, then returned to
SDDM, unloaded the module and disarmed the watchdog. The earlier experimental
gates below describe development history, not the current capability boundary.

The root runner explicitly opts UID 1000 into the trusted lab render node using
the read-only `render_trusted_uid` module parameter and a temporary device ACL.
No process capabilities are granted to Hyprland or applications. Default driver
access remains root-only; this opt-in is not a claim of an isolated production
GPU service. The live GEM and VM budgets are one GiB per client, including
cached allocations and executable aliases. Object and mapping count limits
remain enforced, and quota errors now report usage.

The user compositor starts through `start-hyprland` with its existing config,
theme and normal Omarchy autostart. The experimental wrapper disables borders
and animations and sets scale 2 for the retained framebuffer's synthetic EDID.
Only `omarchy-launch-shell` gets `QT_QUICK_BACKEND=software`: Quickshell's panel
and wallpaper are rasterized by Qt, then composited by the accelerated Hyprland
renderer. Its own GPU path currently faults on VA `0x28fdd6e580`; full border
shaders also encounter an unsupported narrow integer control-flow form.
These remain open, as does long-term firmware stability.

Private libraries live under `/opt/m4mesa`, accessible to the desktop account;
the original root-only lab libraries and normal system graphics are retained.
The home launcher starts a maximum one-hour session. Super+Shift+Escape requests
a clean exit; a fresh boot is required for another firmware bootstrap. On exit,
the supervisor stops the experimental user services and restores SDDM so their
imported graphics environment does not leak into ordinary login. Watchdog
renewal still requires real GPU pixels and a responsive compositor.

Mesa fixes `5f1d2827948` and `42c81f8b3d6` prevent new half-precision handlers
from intercepting already-supported full-width reciprocal and mixed-width
comparison instructions. Four reciprocal and seven focused selection tests
pass; both regressions were reproduced locally before their fixes. A broader
pre-existing packer suite aborts in `LocalIndexZeroExtendUsesCopyAndClear`, and
`HalfMaximumRefusesUngroundedShapes` has a pre-existing expectation failure;
the complete compiler suite is not claimed to pass.

## Earlier bring-up gates

`build.py` builds Mesa with Honeykrisp, Zink, EGL, GLES, GBM and X11/Wayland
support in an isolated directory. It does not install system libraries. The
build is native to the machine running it; the x86 host result cannot be used
as an ARM64 driver. `meson devenv` selects the private build at runtime.

From the workspace root, with Mesa's build dependencies installed:

```sh
python3 linux/linux-alpha/drivers/gpu/drm/asahi/g16_lab/tools/hyprland/build.py \
  --mesa mesa/m4-hyprland --output build/mesa-m4-hyprland --jobs 4
```

If libclc / SPIR-V LLVM Translator were unpacked into a private prefix, provide
that prefix's relocated pkg-config directories through `PKG_CONFIG_PATH`.
The build manifest records the exact Mesa commit, working-tree status, target
architecture, feature options and test source hash. The initial clean source
is Aurora `e58d46899eb1`, tracked separately on local branch `m4-hyprland`.

`composite.c` requests a GLES 3.0 context and requires hardware Zink. The default
requires an M4/T8132 renderer string; `--development-gpu` permits another GPU
and explicitly labels the result as development validation. CPU Vulkan/GL
renderers are rejected. The test exercises:

* Repeated RGBA8 texture updates and framebuffer reuse.
* Nearest and linear sampling, premultiplied source-over blending, and scissor.
* EGLImage -> DMA-BUF export -> EGLImage import -> sampled GL texture, with
  the fd and EGLImages closed before using the retained texture storage.
* Eight native EGL fence exports, fd polling, reimport and server-side waits.
* Eight complete 32x32 readbacks against an independent CPU bilinear/blending
  calculation, allowing at most one UNORM8 unit per channel.

It models basic Hyprland window compositing, not the full compositor. It does
not qualify nested Hyprland, display scanout, HDR, blur, rounded corners,
cross-process image import or all Zink/Vulkan requirements.

For development-host validation with an explicitly selected installed ICD:

```sh
python3 linux/linux-alpha/drivers/gpu/drm/asahi/g16_lab/tools/hyprland/run.py \
  --mesa-build build/mesa-m4-hyprland --results /tmp/hyprland-validation \
  --development-gpu --icd /usr/share/vulkan/icd.d/radeon_icd.json
```

On J713, use `run.py` as root with a native ARM64 Mesa build and omit
`--development-gpu`. It always arms and verifies `/dev/watchdog0` at 60 seconds
on J713, even if the development flag is present. It uses no heartbeat, saves
logs and the manifest before disarming on success, and leaves recovery armed
on a timeout, failed test or kernel fault. M4's Vulkan submission path is not
implemented yet, so this is not currently a passing M4 test.

## Recorded result

Development host, AMD Radeon 780M / system RADV, private Aurora Zink/EGL/GLES:
GLES 3.2 context reported; eight frames and 8,192 pixels pass with a maximum
channel error of one. DMA-BUF image roundtrip and all native fence operations
pass. See `../../render/validation/hyprland-zink-development-20260908.json`.
The complete Mesa build (including its Honeykrisp library) succeeds on x86_64.
This does not establish M4 Vulkan, textures, blending or Hyprland acceleration.

## Native M4 probes

The private ARM64 Mesa build now initializes a physical and logical M4 Vulkan
device through the lab DRM interface. `device.c` checks the actual T8132 ID.
Compile with `-DM4_PIPELINE_PROBE` and the generated `triangle_spirv.h` include
directory to additionally compile a graphics pipeline and record an offscreen
256x256 clear/triangle/store pass. Neither variant calls `vkQueueSubmit`.

Run these only through `../run-render.py --render-drm-submit
--mesa-device-probe PATH`, with the private M4 ICD selected. That runner arms
the watchdog first and follows the CPU-only probe with eight known GPU draws.
A failed CPU probe is recorded independently; successful GPU regression and
teardown permit watchdog disarm while the overall run still reports failure.
The first recording attempt compiled the vertex/fragment pipeline but crashed
on a null background shader returned by Mesa. This is diagnostic evidence,
not successful command recording or Vulkan rendering.

`submit.c` is the separate real-submission triangle probe: eight changing
256x256 Vulkan draws, 524,288 checked pixels. `upload.c` executes eight
buffer-to-image meta copies with padded rows at sizes 2,4,16,32,64,128,256,2.
Both use `--drm-submit-test PATH`, never `--mesa-device-probe`, because they
really execute GPU commands. Both passed on J713/25G83 with the watchdog.
The upload result is recorded in `j713-vulkan-upload-20260908.json`.

The native submission backend now removes an object's mappings on the last
handle close, after synchronous retirement, and counts live objects rather
than all historical allocations. An identical cached-BO shadow binding is
idempotent. The older memory-only mode retains its explicit binding lifetime.
`g16submittest.c` now retains its handles through submission and checks that
closing them removes those mappings; its updated hardware regression remains
to be run. Existing earlier validation records describe the old contract.

Compile `upload.c` with `-DM4_UPLOAD_FRAMES=32` and run with
`--drm-frame-budget 64` to exercise queue-ring wraparound. The runner sends an
explicit stop after the client passes, while preserving the 20-second service
limit and 60-second watchdog. J713 run `20260908T004234Z-render-ta` passed all
32 submissions and clean teardown; both 16-entry command rings wrapped twice.

The compositing test's optional `--local-textures` mode isolates rendering from
PRIME import/export and explicitly reports `egl_dma_buf_roundtrip: false`.
The default remains the complete shared-image gate. The render runner accepts
`--drm-client-mode local-composite` or `composite` for their argument conventions.
