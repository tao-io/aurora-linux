# Fragment-only render dependencies

The G16 runtime historically chained each TA behind the previous command's
final engine stamp, including the preceding fragment pass. Userspace may now
set render flag bit5 when its predecessor dependencies target fragment stages
only; feature bit6 advertises support. Old userspace retains serial behavior.

The runtime drops that TA wait only for adjacent renders in the same prepared
packet, without start timestamps. Each engine has explicit predecessor dependencies, and each
fragment command still waits for its own TA. Thus preceding fragments remain
ordered; compute, other clients, packet boundaries, and first commands retain
the full dependency. Retained per-pass parameter/private-memory owners and
partial-render dependencies are unchanged. This is conservative stage overlap,
not general barrier-index scheduling or concurrent client execution.

Mesa independently limits requests to an explicit fragment-only API barrier,
a preceding graphics stream without arbitrary shader memory writes, the same
command buffer, and no start timestamp. Its opt-in HK_M4_EARLY_TILING defaults
false until hardware qualification.

Validation before hardware: incremental arm64 module build including modpost
passes;14 existing render/dependency encoding tests pass. The unchanged
scheduling profile found seven eligible transitions in the native Factorio
frame, whose ten passes currently span5.75..5.84ms. Hardware result pending.

## First overlap attempt exposed missing execution ordering

The initial implementation incorrectly assumed that queue publication order
also serialized execution on each engine. Replay121015 timed out in its first
menu frame. Its retained firmware scheduler dump shows four TA and four FS
nodes active, all at their WFI continuations, while the next nodes wait for
free engine slots. The first few smaller passes did overlap and finish; no
5ms/performance success is claimed. The guard retained the coredump and
recovered the desktop by reboot. Normal Mesa still has overlap disabled.

Correct the graph explicitly: TA(n) waits on TA(n-1), FS(n) waits on FS(n-1)
and TA(n). This preserves ordered dependencies while allowing one stage to
run ahead of the other. Compute and packet boundaries keep the original full
predecessor dependency. An extra per-owner barrier occupies metadata+0x440,
separate from stamps and UMA counters; ring capacity already allows three
entries per engine per command. Hardware qualification of the fix is pending.
