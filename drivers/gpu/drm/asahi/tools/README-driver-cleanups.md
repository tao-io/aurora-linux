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
