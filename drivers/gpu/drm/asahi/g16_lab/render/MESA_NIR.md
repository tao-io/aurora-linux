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

## DRM memory frontend

`stage=drm-frontend` registers a root-only `g16-lab` DRM render node. The lab
name deliberately keeps application Mesa probing separate until execution
is connected. GEM_CREATE and GEM_MMAP_OFFSET are wire-compatible with the
pinned Aurora Asahi UAPI, checked by compile-time assertions in
`tools/g16drmtest.c`. BOs use the kernel shmem GEM helpers. VM-private BOs and submission are not yet implemented; private, unpublished
VM bindings were subsequently added (see `AURORA_VULKAN.md`). The probe bounds each BO at
64 MiB and cumulative creation per open at 64 objects / 256 MiB.

`20260907T224149Z`: both write-combine and writeback mappings pass zero-fill
and alias readback (131,072 bytes), foreign handles and mmap offsets fail,
VMAs survive handle close correctly, invalid creation requests and the
per-open budget are checked. Module teardown succeeds. This was a guarded
kernel memory test; no GPU firmware was started and it is not a new GPU
execution result.


## SPIR-V shader input

`tools/g16mesa.c` now also accepts vertex/fragment SPIR-V files. The checked-in
`g16_mesa_graphics.h` is generated from `tools/triangle/triangle.vert` and
`triangle.frag` using glslang, Mesa SPIR-V-to-NIR translation, explicit lowering
to the lab vertex fetch / UVS / tile-store ABI, then the AGX3b backend.
Shader arithmetic comes from the input; it is not replaced with canned code.
This is still an offline compiler probe, not vkCreateGraphicsPipelines.

```sh
glslangValidator -V tools/triangle/triangle.vert -o /tmp/triangle.vert.spv
glslangValidator -V tools/triangle/triangle.frag -o /tmp/triangle.frag.spv
# Paths below are from the workspace root:
python3 linux/linux-alpha/drivers/gpu/drm/asahi/g16_lab/tools/g16mesa.py \
  --mesa mesa/aurora --mesa-build build/mesa-aurora-vulkan \
  --output-dir build/mesa-m4-spirv \
  --spirv /tmp/triangle.vert.spv /tmp/triangle.frag.spv
```

The VS is 166 bytes / 16 GPR halves / 14 staging halves; FS is 84 bytes /
8 GPR halves / zero staging halves. Changing fragment RGB to BGR changes
machine code; unsupported flat interpolation and malformed input are rejected.
The no-argument original NIR probe remains byte-exact to its previous output.

On J713/25G83, `20260907T225443Z` runs the SPIR-V-compiled shaders for eight
userspace-paced frames. All 524,288 pixels match, TA and fragment queues finish,
KTrace loses no records, and module removal succeeds. The watchdog is armed
for 60 seconds before module loading and disarmed after evidence collection.
See `validation/j713-spirv-rgb-20260908.json`. This qualifies these shaders on
M4 through the lab backend; Vulkan DRM submission and WSI remain unimplemented.
