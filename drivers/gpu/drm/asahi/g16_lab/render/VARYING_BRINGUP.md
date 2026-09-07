# M4 scalar varying bring-up

The `render_varying` module parameter is an opt-in diagnostic layered on
`render_stream`. Scalar linear interpolation now passes on J713/25G83. The generated
vertex shader reads float4 records `(x,y,color,padding)` from the new input
buffer and exports position plus one scalar. The fragment shader consumes
coefficient zero and packs it into red. A pixel-X read addresses diagnostic
readback only; pixel coordinates do not contribute to the color.

The geometry remains `(-1,-1), (1,-1), (-1,1)`. Frames alternate attributes
`(0,1,0)` and `(1,0,1)`. `render_varying_uniform` instead supplies `(1,1,1)`
and `(0,0,0)` to distinguish coefficient transport from a gradient problem.
Kernel and host compare every pixel, including coverage and target padding.

The working generated-stream regression stays available without these flags.
The committed `tools/run-render.py` preserves the existing platform/firmware
checks, mandatory verified 60-second watchdog, one dispatch per boot, KTrace
capture and evidence-before-disarm ordering.

## Current layout

* Vertex loader: `0x1000014c000`, generated for the relocated main program.
* Vertex main: `0x1000014d800`; the old `0x10000000540` entry is erased.
* Vertex records: `0x1000014d100`, 16-byte stride.
* Coefficient table: `0x1000014d400`, header `0x102`, binding `0x10c`.
  These follow Mesa CF_BINDING_HEADER and CF_BINDING with one user slot,
  one coefficient, linear interpolation and the reserved RHW slot.
* Fragment main: `0x1000014f000` through the generated fragment loader.
* Vertex-input readback: `0x100000a9000`, three words in the existing
  writable readback allocation. Only GPU stores populate those words.
* Fragment coefficient readback: `0x100000aa000`, 256 words indexed by
  pixel X. The selected color varies only with X, so all Y rows write the
  same expected float per column. The uncovered final column retains its
  initial sentinel. This separates interpolation from UNORM packing.

HAL200 PPP fragment state carries the absolute coefficient-table address and
one binding. Output-select bit16 enables interpolation; both VDM and PPP
output counts include all five exported words. The separate PPP record at `0x1000000040` must also declare one linear
f32 varying: its counts word at +4 is `1 << 16`. Leaving it zero suppresses
user coefficient transport even when the VIT and output sizes are correct.
These mappings follow the pinned Mesa implementation and now pass the
scalar M4 test.

The diagnostic readback is emitted after the position COPY waits for the
vertex input load. Its vertex-ID read is asynchronous and the store must
wait for scoreboard group zero. The host checks all three words against the
submitted attributes. Shader staging/exports use Aurora's AGX3b encoder.

## Observations on J713/25G83

* `20260907T214045Z`: linear attributes, f32interp f11=0. Both queues retired;
  correct triangle coverage, black color, no firmware crash. Test failed.
* `20260907T214335Z`: all attributes 1.0, same instruction mode. Same black
  covered triangle; test failed.
* `20260907T214535Z`: f11=1, matching Mesa's higher-level
  `agx3_emit_interp()` for AGX3b fragment-center operations. Still black.
  The raw codec argument is named `perspective`, but Mesa sets this bit for
  linear center interpolation too; VIT selects the shade model.
* `20260907T214834Z`: added vertex-input readback and relocated main program.
  Only the first word changed; its store omitted the dependency wait on the
  immediately preceding vertex-ID read. This observer did not establish
  that the other vertices were missing. The next build adds that wait.
* `20260907T215059Z`: the corrected observer reads exactly `(0,1,0)` from
  the three vertices. Vertex input loading and indexed diagnostic stores
  are established. The fragment target remains black; interpolation is
  still unqualified. The next diagnostic records its unpacked float output.

* `20260907T215354Z`: raw fragment coefficient readback is zero in every
  covered column. The failure occurs before UNORM packing.
* `20260907T215606Z`: binding coefficient zero to RHW slot zero produces
  exact 1.0 readback and an exact red triangle. Both queues retire cleanly.
* `20260907T220233Z`: setting the previously omitted PPP linear-f32 count
  to one produces the expected user-color gradient. All 65,536 pixels,
  target padding, input attributes, queue retirement and KTrace checks pass;
  module removal succeeds. No staging-allocation change was needed.
* `20260907T220340Z`: eight frames alternate `(0,1,0)` and `(1,0,1)`
  with fixed positions. All 524,288 pixels pass, with eight TA/3D starts
  and completions, no lost KTrace records and successful module removal.

These runs stopped firmware, saved KTrace and resource snapshots, then
disarmed the verified watchdog. Only the final run qualifies scalar linear
user interpolation; the RHW run qualifies the system-value binding. Full
local artifacts are under workspace `_provenance/omarchy-m4-render/hardware/`;
compact failure manifests are retained in `validation/`.

Run only after a fresh normal Linux boot, with the matching module and the
runner's `g16image.py`, `g16ktrace.py`, and `experiment_watchdog.py` helpers:

```sh
python3 /root/run-render.py --module /root/g16_lab-varying.ko \
  --results /root/g16-render-results --firmware-ktrace 3 --render-varying
```

## RGB interpolation, 2026-09-08

`--render-varying-rgb` supplies float4 `(x,y,r,g)` inputs. The vertex shader
computes `b = 1-r-g` and exports position plus RGB. There are seven UVS words,
three linear f32 user outputs, and one three-component coefficient binding:
PPP counts `0x30000`, VIT header `0x304`, binding `0x10e`, output count seven.
The six-slot AGX3b scheduler exports the first six values, then waits for slot
one before recycling it for blue at a distinct staging address. The existing
loader allocation remains unchanged. The fragment shader reads coefficients
0,1,2 directly and packs RGBA8; it does not read fragment coordinates.

Frames alternate red/green/blue vertices with green/blue/red vertices, holding
positions fixed. Vertex red-attribute readback and every target pixel are
checked. The scalar coefficient diagnostic remains exclusive to scalar tests.

* `20260907T220509Z`: original generated-stream regression, eight frames,
  all 524,288 pixels pass after the scalar count fix.
* `20260907T220647Z`: RGB one-frame test, all 65,536 pixels pass.
* `20260907T220816Z`: eight RGB frames, all 524,288 pixels pass.

All three runs report clean queue retirement, no lost firmware trace records,
successful module removal and evidence collection before watchdog disarm.
The host RGB oracle also matches a direct barycentric calculation for both
color configurations (131,072 pixels). These qualify this bounded lab draw,
not a public Vulkan implementation, arbitrary shaders, textures or WSI.
