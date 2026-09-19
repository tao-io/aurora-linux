# Fragment-only scope through intervening compute

The new feature bit7 and render flag6 explicitly extend flag5 through compute
commands in one submitted packet. Old flag5 retains its original immediate
render-predecessor meaning; old kernels/userspace do not silently opt in.

For renderA -> computeB -> renderC, with this permission onC:
- B still waits for A's fragment completion.
- C's TA waits for A's TA, so it can overlap A's fragment and B.
- C's fragment waits for B AND C's TA. Neither completion can be omitted.
- Different packets, timestamp starts, and commands without both flags retain
  the previous full predecessor. Flag6 without flag5 is rejected.
Both physical PB ownership and explicit engine predecessor chains from
79f480d290c9/c40864f9d5f8 remain mandatory.

Mesa opts in only for its existing nonempty fragment-only API destination
stage set, without arbitrary preceding shader writes. The new option
HK_M4_COMPUTE_EARLY_TILING defaults false pending replay/live qualification.
Kernel+Mesa cross builds pass. Hardware results follow separately.

Unbounded replay144916 preserves pixels/counts but takes6.93..7.10ms,
versus6.15..6.29ms disabled (144952). First FS waits~0.9ms after TA.
A two-render lookahead window (archived patch) passes the same pixels/counts
in150130, but still takes6.40..6.58ms. Its first FS gap is~0.36ms.
Do not enable either experimental policy by default. The window patch is
removed from active source; it was not a useful performance improvement.
All runs guarded with watchdog60s and six thermal sensors; no GPU fault.
