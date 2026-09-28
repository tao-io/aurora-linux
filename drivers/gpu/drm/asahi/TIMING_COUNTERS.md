# Native retirement timing counters

`/sys/kernel/debug/asahi-m4/timing` (M3: `asahi-m3/timing`) is a
read-only diagnostic snapshot, version 1. It does not read registers or acquire
the runtime lock. A contended snapshot reports `busy=1`; retry later. Readers
must reject unhealthy snapshots and differences across generation changes.

Counters accumulate since runtime creation, modulo 2^64. Each engine row
counts native retirements, commands, nonzero GPU timestamp samples, CPU
preparation time, publication-to-observation time, GPU timestamp span, tiling,
fragment and positive inter-stage gap, all times in ns. Overlapping stages
contribute zero to the gap. M3 retirement may cover a batch; M4 counts one
command per retirement. These are global counters, including desktop work.
Spans include gaps and can overlap across commands/engines; their sum is NOT
GPU utilization or exclusive shader time. Missing timestamps contribute zero;
use nonzero_gpu_samples to assess coverage. No per-process attribution.

The serialized native runtime is the sole writer. Atomic fields and bounded
sequence validation avoid data races and unbounded readers. No additional
firmware commands or timing reads are introduced: values already collected at
normal successful retirement are reused. Faulted/incomplete work is excluded.

Host source-extraction test: workspace `tools/test-timing-stats.py` exercises
coherent concurrent reads, separate engine/batch counts, bounded busy handling
and negative CPU-duration clamping. Source builds and hardware qualification
are recorded separately in the Mesa roadmap evidence.
