# Batched kernel SG mapping, J713, 2026-09-21

**Removed from the default build.** This records the measured candidate and
its patch; see Disposition below.

The 4K SuperTuxKart Zink race spent substantial CPU time constructing command
owners, particularly checked page-table walks and cache maintenance. A fragmented
32 MiB GEM allocation can have 2048 DMA segments. Mapping each segment restarted
the checked root-to-leaf walk and cleaned/published a small PTE range separately.

`VmInner::map_node` now collects the pinned object's selected DMA pages into a
small temporary KVVec (vmalloc fallback for large mappings), validates the whole
source subrange, reserves the complete destination, and calls
`map_scattered_pages` once. This method preflights every destination before any
leaf store and publishes each contiguous leaf-table span through the existing
noncoherent cache-clean/barrier path. It retains checked physical table borrows,
VM locking, permissions, TLB invalidation and allocation-before-publication.
Truncated scatterlists are rejected before partial leaf installation.

This affects driver KernelMappings; GPUVM userspace mapping, repeated-page and
MMIO mapping paths retain their existing implementation. The 32 MiB private pools
and 128 MiB per-queue/per-engine idle cache budgets are unchanged.

Module SHA256: `b4c81ec44d03c82efbf775ace9992f947816aff78b3cd41f75a0df492fe2d11a`.
Boot ID: `2b7906fb-ed36-44b4-b7f2-eddb0679d4b1`.
Stage: `20260921T065834308883Z-stk-batched-map-final-stage`.
Guarded reboot: `/root/asahi-evidence/stk-batched-map-final-reboot-20260921`.

## Validation

- Incremental arm64 module build passed.
- `tools/test_g16_scattered_map.py` compiles the actual production SG-collection
  loop and scattered-mapping function against a sparse leaf-walk model. It
  verifies exact physical addresses, SG offsets/lengths/overflow, page-table
  boundaries, late collisions, missing tables, invalid source lengths/alignment,
  empty/reversed ranges, and no publication on rejected input.
- All 162 targeted Vulkan 1.4.6.2 cases passed, two workers, 6.490 seconds runner,
  9.118 seconds guarded elapsed, peak 81 C. Includes dynamic UBO rendering and
  descriptor copies, regular/alias memory binding, full 1 MiB mapping, and host
  write-to-shader/vertex barriers. Frozen indices and catalog hash are retained.
  Evidence: `20260921T070115857120Z-stk-batched-map-cts`.
- Earlier equivalent batched-PTE build using KVec instead of KVVec passed the
  three-frame Zink replay with exact baseline pixels:
  `20260921T065628589537Z-stk-zink-batched-map-replay`.
- The existing G17 source-string check `test_g17_render_vm_handoff.py` fails at
  line 116 on both the unchanged 5154e52a6aa7 tree and this tree. Its expectation
  for four occurrences in `g17_resources.rs` is unrelated and predates this
  change; that file is untouched. This is recorded, not counted as a pass.

The hardware guard retains watchdog60, 0.5-second six-channel thermal monitoring,
95 C stop, full requested CPU ceiling under the existing thermal policy. Normal
SDDM autologin and both displays are restored between bounded experiments.

## Performance and final replay

Clean Zink race: 770 frames / 61.987 s = 12.422 FPS, peak 86.75 C,
`20260921T070204834144Z-stk-zink-batched-map-race`. Across 10749 4K passes,
mean kernel preparation fell to 2.388 ms/pass from 2.775 ms/pass with the
GPU-only mapping change, and 3.366 ms/pass before either change. This is a
measured CPU-cost reduction, but no additional Zink FPS gain was demonstrated
(the preceding profiled run was 12.771 FPS).

Native race: 914 / 46.289 s = 19.746 FPS, peak 87.75 C,
`20260921T070443264367Z-stk-vulkan-batched-map-race`, versus 22.605 earlier.
Mean preparation also improved (3.146 -> 2.593 ms/pass), but live fragment spans
were longer. A fixed native three-frame replay on this exact final module,
`20260921T070747727510Z-stk-native-batched-map-replay`, remains 23.948 ms median
GPU time and exact pixels against the earlier reference. The single live race
is slower; the replay does not establish live non-regression. Retain both
measurements rather than attributing all FPS variation to table mapping.

All races use unchanged quality at 3840x2160 internally, downsampled to the
1920x1080 fullscreen monitor. Menus are excluded. No full CTS claim is made.

## Disposition

Removed from the default build after live results failed to establish an FPS
benefit. The measured CPU-cost reduction and exact replay parity remain valid,
but do not override the live-race results. The complete candidate patch and
focused test are retained here; run the test against the candidate checkout
at 8ae9ec7e71e2, not the default source after rollback.
