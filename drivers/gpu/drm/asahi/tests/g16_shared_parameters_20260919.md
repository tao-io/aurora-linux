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

Hardware: bootfba14ca7, modulefc943ef6. Private-pool control152212 matches
every pixel and all ten counters at6.156..6.254ms. Shared-pool replay152300
also matches, and logs exactlyone init per ten scenes, but6.844..6.919ms
is still slower than the private normal path. Unbounded private compute
overlap had taken~7ms. Neither experimental path should be the default.
Watchdog/thermals passed, peak67.25C. No live qualification claimed yet.

Eight targeted synchronization2 cases152624 allPass (fragment/vertex consumers
after fragment/compute writes), peak73.5C. Full CTS still deferred.

Next experiment: independent tiling behind the most recent full-order TA
anchor. Merely removing the immediate TA chain is UNSAFE with cumulative
completion stamps: a later TA can finish first and satisfy an earlier wait.
Each shared-pool scene therefore owns an entire retained ClientNotifications
object (not just private stamp cells in one coalescing notifier). Publication,
barriers, retirement and fault capture all select that scene's notifier.
New stamps are primed to event-0x100 before publication to preserve signed
comparison ordering across wrap. Compute and default render notifications
remain unchanged. FS still waits on the immediate previous command and own TA.
The TA anchor advances on every full-order render; it keeps all inherited
non-fragment dependencies. Packet boundaries remain fully ordered.
Hardware qualification of independent completion/tiling is pending.

Independent replay153314 failed with host retirement timeout, job119. All
three scheduler queues are idle, no active nodes; the stalled TA's firmware
stamp is10b00, but userstamp remains primed10a00. Its fragment firmware AND
user stamps are10b00, and USC private-pool submitted/completed areboth4.
TA and FS timestamps are complete. The shared PB descriptor matches the
manager and all engine selectors. DumpSHA
91d6a6b28298350a627c52163b1a0112ab42335255ab8422ec849a34f515fcc9.
Guard recovered desktop; peak67C, no thermal cutoff.

Retirement correction: when the exact own-fragment stamps BOTH equal the
render event and USC completed equals submitted, accept that stronger proof
in place of a missing TA userstamp. TA firmware completion and the existing
queue/pipe checks remain mandatory. OwnFS has an explicit barrier on ownTA.
This is not accepting queue-idle alone or ignoring a timeout. Build passes;
replay qualification pending. Original failure remains recorded.

Retirement fix754cccf0ab3b replay153923 passes allfiveframes, everypixel and
alltenquerytotals. WholeGPU6.716..6.890ms, stillslowerthan normal6.2ms.
Peak73.5C, watchdog/thermalsPASS. Doesnotjustifydefaultenablement.
