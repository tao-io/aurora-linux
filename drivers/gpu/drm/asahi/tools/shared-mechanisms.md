# M3/M4 mechanism and ownership audit

2026-09-24. Scope: qualified J514S/T6030 G15/V14_8_3 and J713/T8132
G16/25G83 paths. The immutable source trees and helper hashes are in
`shared-mechanisms.json`. This is a source/contract audit, not another hardware
qualification. Separate kernel branches still contain their own source copies;
a matching file is not an implicit dependency between the branches.

## Reuse already present

| Mechanism | Actual consumers and decision |
| --- | --- |
| Geometry | `m3_render::geometry`, M3 tiler/fragment encoders and M4 render use `g16_render::Geometry`. The whole module is identical in the audited trees. Retain this common calculation rather than introduce another geometry implementation. |
| Compact pointers and program inputs | M3 submit/tiler/fragment use `g16_render_state::compact`, `Program` and `DepthStencil`; M4 uses them in render-state construction. These selected items are byte-identical. The whole state module differs: M4 added the parameter-buffer owner ID to register emission. Do not replace the M3 module wholesale. |
| Memory-range attachments | M3 submit/compute and M4 engine jobs use `g16_attachments`. The complete module matches. Range validation, 128-byte normalization and record encoding stay shared; locations in engine microsequences are device-specific. |
| Hardware resource descriptions | `m3_resources` reexports the `g16_resources` Region/Resources types; M4 uses them directly. That module matches. Device-tree resolution and firmware admission still have device-specific consumers. |
| Publication barrier | Both paths call `g16_memory::publish`; the function matches (`dsb oshst`). The Buffer implementations and their allocation contracts do not match. Sharing the barrier does not make the allocators interchangeable. |
| Qualification dispatch image | `g16_dispatch.rs` matches as a source artifact. Do not infer that every current M3 production submission consumes this module, or that sharing a small qualification program proves complete ISA compatibility. |

The two M4 engine families now consume one timestamp-operation layout and one
private-memory descriptor layout. Render and compute dependency patching also
uses one barrier-field definition. These are within the already-qualified M4
ABI; they do not import the G15 firmware layout.

## Boundaries retained deliberately

- **Microsequences:** M3's timestamp records occupy 0x3c bytes, separated by
  a four-byte wait (compute starts at 0x1bc/0x1fc, finalize at 0x238). M4's
  records occupy 0x4c bytes (compute 0x1bc/0x20c relative to the embedded
  sequence, finalize 0x258), with extra fields and a different UUID location.
  Their similar opcodes do not justify a shared wire struct or patch offsets.
  Start/finalize, notifier and queue layouts remain explicitly versioned.
- **Memory ownership:** M3's `m3_memory::Buffer` owns coherent DMA backing,
  retained subpage offsets and explicit firmware/client mappings. M4's
  `g16_memory::Buffer` owns GEM backing and retained aliases. M3 now records
  per-pass and cached-view ownership directly, but preserves its fixed arenas.
  Pool replacement, mapping permissions, firmware caches and ASID lifetimes
  must agree before attempting allocator sharing.
- **Address types:** M3 has distinct checked firmware references, regions and
  lower-root GPU byte addresses. Its GPU subobject type admits four-byte
  alignment. M4 initialization distinguishes physical MMIO from mapped
  firmware addresses; compute page-owner validation requires 16-KiB alignment.
  A generic integer wrapper with one alignment rule would erase these contracts.
- **Register emission:** geometry and compact-pointer math are reusable; the
  owning command sizes, register placement, mirrored fields and parameter
  identity differ. Preserve per-generation encoders and their independent
  retained-source/template oracles.
- **Unknown fields:** native constants retain their firmware/board scope and
  evidence references. `UNKNOWN_*` is intentional. Naming or moving a field is
  not reverse engineering its semantics. In particular UMA+44 is diagnostic
  unknown state; the retired pool consumption reader uses cursor +20.

No new cross-device runtime abstraction is introduced by this audit. Existing
common mechanisms already cover the contracts established on both devices.
The remaining byte-write helpers are small implementation details under
different validation and mutation rules; merging them would not establish a
shared firmware ABI. Any future common runtime change must pass both devices'
retained checks and guarded application evidence.

## Behavioral follow-up sequence

These are separate work items from the representation migration. None is
silently enabled by removing templates or naming offsets.

1. **M3 parameter-pool sizing/growth.** Start from the current 256-MiB bounded
   pool, retaining the evidence that the earlier 64-MiB pool stalled large TA
   work. Measure page high-water marks and identify the G15 partial-render and
   append/rebuild protocol before reducing the initial pool. Test exhaustion,
   append, displaced-owner reuse and allocation failure with retained pixel
   references; keep all old backing alive until exact retirement is proven.
2. **Fixed arenas and relocations.** Classify each fixed VA as firmware
   requirement, compact encoding constraint or implementation choice. Move one
   owned family at a time while preserving subpage offsets, permissions and
   all firmware self-references. Gate on host bounds/overflow rejection,
   baseline replay pixels, repeated reset/reuse and fault evidence. A template
   no longer being read is not permission to move firmware-cached storage.
3. **Cached client views.** Preserve the current active-plus-three-cached VM
   policy and mapping-before-VmBind drop order. Instrument residency and eviction
   before changing it; test repeated client alternation, eviction and allocation
   failure with distinct payloads at reused virtual addresses. Confirm ASID and
   mapping ownership through retirement, not merely successful submission.
4. **Pass limits and concurrency.** Retain M3's sixteen pass slots and existing
   retirement checks. Changing the limit or overlapping additional submissions
   needs separate ring-wrap, stamp-wrap, dependencies, timestamp and VM-switch
   evidence. On M4, retain the already-qualified hardware pool-slot and batch
   policy. Similar shader ISA does not establish concurrent firmware scheduling.

All future hardware segments retain the 60-second watchdog, all-channel
0.5-second telemetry, 95 C startup gate and the explicitly authorized
110/115/120 C development policy. Broad CTS or long stress runs are not a
substitute for a minimal failing replay. Normal accelerated login and autologin
must remain available after experiments.

## Reproduce this audit

The M3 branch versions `tools/asahi/audit-shared-mechanisms.py`. From the
workspace, its `tools/m3-gpu/audit-shared-mechanisms.py` symlink accepts `--m3`,
`--m4`, both `--*-revision` arguments and an exclusive `--output` JSON path.
It reads only immutable Git blobs, records complete module hashes and extracts
selected common items. Hash equality is source evidence; the consumer and
lifetime decisions above come from inspection of the actual runtime paths.
