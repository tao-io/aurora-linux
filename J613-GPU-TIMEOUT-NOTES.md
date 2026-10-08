# J613 D3h timeout diagnostics, 2026-10-08

Parent: be0f3ccd5938a40d769535464b18c378a7296b96, published D3h haptics/transport source.

This branch fixes the proven pstate printk flood and adds bounded timeout retirement witnesses. It does not claim to fix the original hang, change DVFS, relax retirement, reclaim unretired DMA owners, or port firmware recovery. Default GPU debug is off. `debug_flags` GPU bit 8 opts in to pstate transition logs; aggregate SubmitSummary bit 24 already contains pstate sample counts. Thermal ceiling changes and faults remain visible.

Job 422400's fragment user stamp was 0x671ff00, firmware stamp 0x671fe00, with ordered TA/fragment timestamps and UMA completed=submitted=0xdf44. The driver's exact firmware-stamp predicate then escalated to global failure. A false timeout from lagging notifier publication is the leading explanation. Later firmware barriers consume firmware stamps, so simply removing this predicate could introduce dependent-work hangs or DMA lifetime violations.

Validation: diff whitespace check and decoding the original sealed evidence. No kernel build, RAM test, stage2 package, hardware qualification or installation is claimed. Prepare a diagnostic package only in a separately allocated build workspace with unchanged D3h configuration, complete modules and actual cpio-length verification. Do not stop neo-build.

Research and replayable decoder: satchlj/mac-aurora-omarchy, machines/m3-air/research/2026-10-08-d3h-gpu-timeout/.

NEED OWNER: coordinate a diagnostic RAM window after an offline build and coherent module/package verification. No installation or reboot was performed.
