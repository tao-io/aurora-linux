# Bounded private GEM shmem reclamation (J713, 2026-09-16)

Rust shmem GEM release now holds one private backing-file reference across
`drm_gem_shmem_release`, then consumes that reference synchronously. DMA mappings,
page pins, reservation locks and GEM metadata are released first. Imported
objects have no private shmem file. This changes final destruction only; live
GPU commands and firmware-retained resources keep their ordinary references.

The final shmem file reference previously reached fput from a GPU completion
worker. Linux queued eviction to the single delayed_fput worker. Twelve CTS
clients rapidly allocating and retiring private pools outpaced that worker:
1000 one-case processes grew Shmem to 8.2 GiB, even with only about 12 live CTS
clients. A 4704-case run exhausted memory. Repeated read-only worker snapshots
caught delayed_fput -> __fput -> shmem_evict_inode -> shmem_undo_range; stopping
the workload allowed the backlog to drain. No new native GPU retirement timeout
was found in the allocation-failure evidence. VM errors are retained separately.

The identical 1000-case prefix passes before and after: 392 Pass / 608 NotSupported.
Before: 30.501 s, sampled Shmem peak 8249 MiB, minimum available 4547 MiB, peak 64.75 C.
After: 35.576 s, sampled Shmem peak 1701 MiB, minimum available 11009 MiB, peak 64.75 C.
This is a reclamation fix; these runs do not establish a performance improvement.

Evidence:
- Before: 20260916T153212995415Z-dx-image64-formatless-memory-r2.
- After: 20260916T154230554149Z-dx-image64-formatless-memory-fixed-r1.
- Initial full failures: 20260916T151221417464Z-dx-image64-formatless-r1 and
  20260916T152136878887Z-dx-image64-formatless-r2 (second deliberately stopped).
- Fixed module SHA256: a5affc7dcc90cd7b29ae230ff7324fbd8712d9aadab7419882038e16b95a4f86.
- Experimental Mesa SHA256: a9f1dbedf5de0d495cc67f9ada0376562c08219b7409fa3f9b8e51b36b58ff0f.

All hardware execution uses a 60-second watchdog, 0.5-second six-channel thermal
monitor, 95 C cutoff, 12 CTS workers and 60-second case limit. Clock ceilings were
requested at full values; CPU thermal policy remains active. Reboot was guarded.
Frozen catalog identity and case global indices are retained.

Full formatless qualification: 4704 cases, 1968 Pass / 2736 unsupported
dependencies, no failures, 178.796 s, peak 70 C. Sampled Shmem peaks at 1716 MiB
and available RAM never falls below 10819 MiB. Evidence:
20260916T154327737064Z-dx-image64-formatless-r3. Default combined regression: 6060 cases, 3028 Pass / 3032 unsupported
 dependencies, no failures, 255.640 s, peak 62.75 C. Sampled Shmem peak 3302 MiB,
 minimum available 9095 MiB; memory remains bounded across the broader workload.
 Evidence: 20260916T154722438094Z-dx-image64-formatless-default-regression.
 All 108 independent buffer/shared/image/formatless/alias/scratch configurations
 pass, peak 59.75 C: 20260916T155207393115Z-dx-image64-formatless-default-oracles.
 Default Mesa SHA256:
 37d5e4283c8c15559cc8f76aab6ab5dacac9a0a74ed3f83a97096b7dbd51d382.
