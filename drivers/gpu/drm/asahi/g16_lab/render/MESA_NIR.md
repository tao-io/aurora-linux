# Mesa NIR compiler bridge for J713

The host probe `tools/g16mesa.c` compiles the RGB lab draw through the pinned
Aurora Mesa NIR compiler, including optimization, register allocation and
AGX3b scheduling. It uses lowered vertex loads/UVS and tile-store intrinsics,
with normal NIR fragment interpolation lowered by Mesa. This is a compiler
bridge, not a SPIR-V frontend or public Vulkan device implementation.

The vertex shader consumes float4 `(x,y,r,g)` via `u0:u1`, computes blue as
`1-r-g`, and exports position plus RGB. The fragment shader consumes one
three-component noperspective binding. The probe rejects unexpected uniforms,
constants, scratch, preambles, entry offsets, allocation sizes, or coefficient
layouts before writing the generated header. Current outputs are 166-byte
VS (14 GPR halves, 14 staging halves) and 84-byte FS (8 GPR halves, no staging).
The lab uses the existing qualified loaders and matching VIT/PPP state.

## Reproduce on the build host

Configure Mesa with `-Dvulkan-drivers=asahi -Dgallium-drivers=zink
-Dtools=asahi -Dbuild-tests=true`. This session's host build additionally used
`-Dplatforms=[] -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled
-Dgles2=disabled -Dllvm=enabled -Dvideo-codecs=[]`. It builds the Honeykrisp
library and Zink code for host inspection, not an installed ARM64 desktop.
Missing libclc 22.1.8-2 and SPIR-V LLVM translator 22.1.5-1 packages were unpacked
under workspace `build/mesa-deps/root`; no system packages were changed.
Their relocated pkg-config files must be on `PKG_CONFIG_PATH` at setup.

After `ninja -C build/mesa-aurora-vulkan`, run from the workspace root:

```sh
python3 linux/linux-alpha/drivers/gpu/drm/asahi/g16_lab/tools/g16mesa.py \
  --mesa mesa/aurora --mesa-build build/mesa-aurora-vulkan \
  --output-dir build/mesa-m4-nir
```

The wrapper reuses Mesa's graphics-test compile/link flags and libraries,
without modifying its source or Meson files. It verifies the clean pinned
revision, writes the generated header, and records source/output hashes and
exact build commands. Copy the generated header into `render/` only after
reviewing the compiler report. The checked-in header needs no Mesa dependency
at kernel/module build time.

## Validation boundaries

The complete host build succeeds. The custom RGB probe succeeds and reproduces
the checked-in bytes. The reference's broader `agx3_graphics_compile_test`
fails on four observations: AGX3b instanced varying rejection, flat-color
binary expectation mismatch, discard unexpectedly accepted, and AGX3c meta
RGBA upload rejection. These are recorded, not waived as full-suite success.
Full logs are in workspace `_provenance/aurora-mesa-build-20260908/`.

`--render-mesa-fragment` and `--render-mesa-vertex` independently replace the
RGB lab stages. The compiler's VS has no diagnostic store: its readback must
retain the sentinel, while submitted attributes, every rendered pixel and
firmware retirement remain mandatory checks. All hardware runs use the
verified 60-second watchdog and a fresh boot.

* `20260907T221335Z`: compiled FS plus generated VS passes all 65,536 pixels,
  clean queues/KTrace and successful module removal.
* `20260907T221509Z`: compiled VS and FS together pass all 65,536 pixels,
  clean queues/KTrace and successful module removal. The compiler VS leaves
  the unused diagnostic readback at its sentinel as expected.
* `20260907T221657Z`: compiled VS and FS, eight alternating RGB frames,
  all 524,288 pixels pass. Eight TA and fragment jobs start and finish;
  KTrace has no lost records; module removal succeeds. The watchdog was
  verified at 60 seconds and disarmed only after evidence collection.

## Retained worker gate

`--render-service` runs the eight qualified draws from a worker, with the
firmware lifetime spanning userspace submissions. The root-only debugfs
`g16_lab_render_submit` file accepts the next sequence number and waits for
that draw to finish. It accepts no GPU pointers or unqualified commands.
The worker services firmware during idle time and has a 20-second total
budget, independent of the mandatory 60-second hardware watchdog. The
`stopped` status is published only after firmware stop and mapping restoration.

`20260907T223642Z`: no frame during initial idle, four invalid submissions
rejected, eight userspace-paced frames verified (524,288 pixels), clean
firmware retirement and successful module removal. This is a lifetime gate
for the future DRM backend; it is not yet Vulkan command submission.
