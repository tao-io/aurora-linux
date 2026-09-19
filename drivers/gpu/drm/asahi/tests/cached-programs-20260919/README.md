# Bounded firmware-private microsequence experiment

The common Asahi render queue places microsequences in kalloc.private
(PROT_FW_PRIV_RW). M4 currently uses firmware-shared, uncached storage.
This opt-in diagnostic measures immutable private copies without assuming that
reusable firmware cache maintenance already works on G16.

DRM_ASAHI_RENDER_CACHED_PROGRAM_PROBE / HK_M4_CACHED_PROGRAM_PROBE uses one
fresh 16 KiB page per stage in a lazy 16 MiB arena. AP publishes each page once;
firmware may modify its own state. The entire arena survives until ASC stops.
Capacity exhaustion fails with ENOSPC. This is NOT a production allocator or
an option for an unbounded application run. It defaults off.

RunCommand sequence pointers and the finalizer's UMA self-reference relocate;
external command, queue, metadata and user timestamp pointers remain unchanged.
Host wire tests cover both stages and all supported timestamp fanout lengths.
MinimalTimestamps and cached programs cannot be combined.

TODO if measurement warrants production use: qualify G16 FWCTL/cache maintenance,
reusable cached allocations, retirement and recovery. Do not recycle pages or
unmap them while ASC runs. A fault dump retains the first 4 MiB of AP-visible
program data, which is not necessarily the firmware's cached state.

Build and 16 host wire tests passed. Hardware measurement pending.

Hardware: 171104 cached ON and 171129 OFF both reproduce exact pixels and
all ten query results. Complete-frame median 5.994625 ms ON, 6.083208 ms OFF
(five frames each). This small difference does not justify enabling a bounded
allocator or establish a robust production gain. Normal desktop remains OFF.
