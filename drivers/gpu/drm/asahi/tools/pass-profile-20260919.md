# Self-contained render retirement records

SubmitTiming now emits G16G_PASS with the retiring packet/index, ASID, geometry,
submitted VDM address and TA/gap/fragment durations from that exact owner.
Prepare-ahead can interleave owned-render messages before earlier retirements;
joining those messages by adjacency is invalid. Summary counters were already
read from the actual owner and are unaffected. This opt-in logging makes no
submission or synchronization changes.

Built with the J713 LLVM/Rust configuration. The workspace helper
tools/gpu-pass-profile.py joins Mesa HK_M4_SHADER_PROFILE records by submitted
VDM address within an explicitly selected ASID. Multiple shaders/address reuse
remain an observed set, not individually attributed GPU cost. Synthetic checks
cover interleaving, ASID separation, reuse, signed stage gaps and tail selection.
Hardware capture 20260919T054025709713Z passes (30-second menu, peak 87.75 C).
The follow-up 054302402085Z with Mesa BEGIN/SUBMIT boundaries pairs 8,401
retirements to recorded submission lifetimes, including recycled addresses.
Only equal complete per-address sequence counts are paired; mismatches remain
explicit unions. This is diagnostic timing, not a throughput qualification.
