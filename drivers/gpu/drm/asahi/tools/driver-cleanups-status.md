# Driver cleanup checkpoint

2026-09-24. Implements the representation work in
`M3_AND_M4_DRIVER_CLEANUPS.md` for the qualified M3 Pro J514S and M4 J713 paths.
This checkpoint does not expand firmware versions or claim Vulkan conformance.

## P0 — reproducible inputs

The M3 Git-tree checker audits all 1,204 historical schema dependencies,
allocation extents, provenance hashes, relocation bounds and batch maps.
Negative cases run in private indexes; the current production check confirms
that none of the four numbered schema modules is registered. Three named
board-data inputs remain explicit and hash-pinned. Generator provenance and
unavailable external inputs are documented in the M3 construction-check README.

M3 had an Image/modules/dtbs clean-export baseline build; later families were
built as Asahi modules from recorded clean source trees. M4 module builds use
checksum-verified Git exports and a prepared kernel output directory, as well
as the installed-release build. They are not claimed as new from-empty whole
M4 kernel builds. Each validation record identifies configuration, source tree,
module hashes and hardware evidence separately.

## P1 — M3 runtime construction

Normal initialization, compute and rendering no longer load the four numbered
construction-template collections. Explicit encoders cover queues, state,
scenes, parameter/UMA managers, barriers, work commands and microsequences.
Per-pass owners replace generated batch-index wiring; shared and compute
storage describe sizes, access, reset behavior and references directly.

The historical binaries remain test oracles. The three named board-data inputs
and Apple runtime firmware remain dependencies; this cleanup does not replace
calibration/firmware with invented constants. Fixed arenas, sixteen pass slots,
retirement policy and cached client mappings preserve the qualified behavior.
The final M3 runtime checkpoint is `c09f7df2bc61`; `2c6589a2f6d3` adds the
read-only cross-device audit and follow-up documentation.

M3's per-family records include clean-source builds, reset/reuse and
thousand-draw slot checks, pinned CTS, the retained replay and normal Plasma.
The final init-storage run retained baseline CTS results (including explicit
NotSupported cases), byte-identical replay output and a visible Plasma desktop.
The overlap counter for that run comes from the same-boot journal because the
short dmesg ring had already evicted earlier messages; this limitation is
recorded in `init-storage-validation.json`.

## P2 — M4 layout contracts

The driver now centralizes render/timestamp, notifier, queue/client-completion,
compute-command/UMA, render metadata/parameter/dependency and initialization
layouts. Producers, runtime patches and readback use their shared definitions.
Semantic inputs remain separate from serialization; packed fields use explicit
little-endian writes instead of unaligned Rust references. Initialization I/O
maps distinguish physical addresses from owned firmware virtual mappings.

Unknown constants retain their qualified board/firmware evidence and explicit
unknown names. Single-use native register values remain alongside their
semantic inputs and producer references. The cleanup does not claim that every
unknown field has been reverse engineered. Existing rejected-input mutation
behavior is preserved, including the compute encoder's late event-slot check.

Actual-source host comparisons cover encoders, runtime patch/readback methods,
initialization and control-ring publication data. Each runtime family has its
own clean-source/module build and guarded boot, retained replay, pinned CTS and
normal Hyprland/Foot evidence. The latest four family records are
`config-layout-validation.json`, `compute-layout-validation.json`,
`render-storage-validation.json` and `init-fields-validation.json` in the M4
`drivers/gpu/drm/asahi/tools` directory.

## P3 — shared mechanisms and separate behavior work

The completed source audit is `shared-mechanisms.md` and its hash record.
Existing common geometry, compact-pointer, attachment, resource and publication
helpers are retained. G15/G16 packet layouts and DMA/GEM ownership are kept
explicitly distinct. No new cross-device runtime abstraction needed hardware
qualification in this final audit.

That document schedules parameter-pool growth, fixed-arena relocation, cached
VM views and pass/concurrency limits as separate behavior changes, each with
specific failure, retirement and replay gates. Feature exposure, performance
policy and firmware-version expansion remain outside this representation pass.

## Evidence and operation

Both machines retain their normal accelerated login configuration and M4
autologin. Every hardware segment uses the verified 60-second watchdog and
0.5-second all-channel thermal monitor under the authorized development policy.
No broad CTS run or unbounded stress workload was introduced.

Workspace evidence is under `_provenance/driver-cleanups-20260924/` and the
referenced `_provenance/experiments/` directories. Source tools, validation
summaries and audit documents are committed; raw captures, screenshots and
kernel modules are retained locally and are not added to Git.
