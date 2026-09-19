# Experimental scene-shared parameter pools

Common Asahi retains one buffer manager across scenes and emits InitBuffer
only on rebind or growth (queue/render.rs). The G16 path previously allocated
and fully initialized a different manager for every render. In compute-stage
overlap packets this queues ten full InitBuffer operations for ten scenes.

Separate parameter storage from scene storage with a retained Arc/Mutex owner.
Every scene retains its pool; tilemaps, preemption state, commands, timestamps
and USC private allocators remain per-scene. A shared packet uses its bank's
first physical PB slot and emits only one full initialization before its first
TA. All physical engine selectors and command PB IDs name that same manager.
The aggregate capacity equals the sum of the previous per-scene allocations.
All existing TA/FS/compute DAG dependencies remain intact.

This is enabled ONLY for the existing experimental flag6 packets, off in normal
Mesa. Each experimental packet receives a new pool; normal displaced-owner
recycling still rebuilds its private pool. A recycled scene replaces a shared
pool rather than resetting storage retained by other scenes. Preparation
failure drops unpublished references; after publication every scene keeps its
pool until the existing retirement/replacement lifetime permits release.
The parameter counter is independent retained storage, as in common Asahi.

This checkpoint has built and passed the existing 15 wire/dependency and eight
native engine-state host tests. Hardware qualification follows; neither shared
pool performance nor live application stability is claimed yet.
TODO: reusable per-VM pool cache, growth, partial-render/exhaustion qualification,
shared pool across packet boundaries, full CTS. New allocation per packet is
appropriate only for this isolated experiment, not default enablement.
