# Driver representation checks

These checks exercise the current M4/25G83 encoders on the host. They are
separate from guarded firmware execution and desktop validation.

From the workspace:

```
python3 tools/check-render-wire.py --kernel linux/linux-asahi --baseline f16bcf4045e2 --output build/wire-comparison
python3 tools/g16-layer-state-test.py --kernel linux/linux-asahi --output build/layer-comparison
```

`check-render-wire.py` is a workspace symlink to the versioned tool here.
It exports the baseline modules from an immutable Git tree, records all source
hashes, and runs the same current fixture against baseline and proposed code.
Six variants cover both engines with different retained firmware addresses,
ASIDs, generations, counters, event slots, stamps and identifiers. Full output
bytes and comparison hashes are retained. This is a regression oracle from
the existing encoder, not independent proof of Apple's firmware ABI.

The wire fixture also checks stats/queue ownership through start and finalize,
and rejection without mutation. Its original version was stale: it omitted
timestamp-storage/peer-fragment inputs and used pre-timestamp sequence lengths
and finalize offsets. The layer fixture omitted parameter-buffer ownership.
Those harness failures are recorded in the workspace cleanup evidence; fixes
change no production driver behavior.

Compile `m4_timestamp_fanout_test.rs` with `rustc --edition=2021 --test` and run
the resulting executable. It imports the actual compute module, including
`EVENT_GENERATION`, instead of substituting an outdated cache generation.
Its tests also cover geometry, compute completion, timestamp fan-out and
rejected inputs.

The current cleanup evidence is under
`_provenance/driver-cleanups-20260924/` in the workspace. Runtime changes still
require a clean-source build, guarded device tests and normal desktop checks.

## Queue and client completion storage

`g16_queue_layout.rs` names the J713/25G83 context, queue, scheduler storage,
ring cursors, bootstrap barrier and per-client completion cells. Queue-owned
buffers and their existing retention policy are unchanged. Unknown offsets
retain explicit unknown names; no G15 layout is reused by inference. Client
notification creation, address publication, threshold writes, stamp priming and
readback share the same three-engine cell definitions.

Run the queue comparison from the workspace:

```
python3 tools/check-queue-wire.py --kernel linux/linux-asahi --baseline e2ff38a41660 --output build/queue-wire-comparison
```

The same fixture executes baseline and candidate queue source in host byte
buffers. It replaces only Arc/Mutex construction with direct ownership; all
queue/client logic and Buffer field accesses remain from the source. Six varied
addresses/ASIDs exercise two ring sizes through wraparound, bootstrap barriers,
consumer retirement, batching, invalid inputs, threshold overflow, reservation
rollback and all three engines' completion cells. Ordered allocation, write,
read and publication logs must match exactly. This checks byte/operation
preservation, not real DMA, locking, ASID lifetime or firmware behavior; guarded
hardware and normal desktop gates remain mandatory.

`queue-layout-validation.json` records both module builds, the six source-code
wire comparisons, guarded normal reboot, 12/12 pinned CTS, byte-identical
2560x1664 replay and normal Hyprland/Foot capture. Every hardware segment held
the 60-second watchdog and 0.5-second thermal monitor; peak was 63 C.

## Initialization allocation and I/O mapping storage

`g16_config_layout.rs` gives each of the 31 J713/25G83 allocations a named
size and states which two also require a client GPU mapping. `Buffer` still
owns the page-aligned allocation and mappings for the entire Config lifetime.
The 19 board-specific I/O descriptions distinguish physical register addresses
from allocated firmware virtual addresses. Their checked 40-byte encodings
preserve the subpage offsets, protected slot, mode/tag values and split MCC
banks. Empty slots in the 42-entry firmware table remain zero. Slot numbers and
unknown mode/tag values are evidence from the qualified producer, not a claim
that another board has the same register map.

```
python3 tools/test-config-layout.py --kernel linux/linux-asahi --baseline e8a21feb3c47 --output build/config-layout-comparison
```

This host check extracts the old allocation and I/O tables from the immutable
baseline, checks all owners and three varied firmware-address sets, and tests
rejected extents, slots and overflow. New records are returned by value, so
rejection cannot alter caller bytes. It does not emulate UAT or firmware boot.
`config-layout-validation.json` records the build and guarded boot/replay gates.
