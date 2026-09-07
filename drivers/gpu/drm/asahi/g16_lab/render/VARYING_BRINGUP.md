# M4 scalar varying bring-up

The `render_varying` module parameter is an opt-in diagnostic layered on
`render_stream`. It is not a qualified rendering feature yet. The generated
vertex shader reads float4 records `(x,y,color,padding)` from the new input
buffer and exports position plus one scalar. The fragment shader consumes
coefficient zero and packs it into red; it does not read pixel coordinates.

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

HAL200 PPP fragment state carries the absolute coefficient-table address and
one binding. Output-select bit16 enables interpolation; both VDM and PPP
output counts include all five exported words. These mappings are derived
from the pinned Mesa implementation and remain subject to M4 validation.

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

All four runs stopped firmware, saved KTrace and resource snapshots, then
disarmed the verified watchdog. None qualifies varying interpolation. Full
local artifacts are under workspace `_provenance/omarchy-m4-render/hardware/`;
compact failure manifests are retained in `validation/`.

Run only after a fresh normal Linux boot, with the matching module and the
runner's `g16image.py`, `g16ktrace.py`, and `experiment_watchdog.py` helpers:

```sh
python3 /root/run-render.py --module /root/g16_lab-varying.ko \
  --results /root/g16-render-results --firmware-ktrace 3 --render-varying
```
