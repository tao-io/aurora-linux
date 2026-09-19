.. SPDX-License-Identifier: GPL-2.0-only

Apple J713 PMP thermal policy
============================

``CONFIG_APPLE_PMP_THERMAL`` provides an opt-in Linux CPU cooling policy for
the validated J713/T8132 board. It does not change PMP firmware, the SMC, or
any firmware protection threshold. The 80 C passive trip is a Linux development policy choice, not an Apple
specification or a hard temperature ceiling.

The module reuses the same read-only record decoder as the ``apple_pmp``
hwmon provider. The six-channel hwmon ABI is unchanged. ``apple-pmp-die``
is an additional thermal zone whose sensor is the aggregate maximum of
all valid documented records, including the normally invalid sixth record.
No individual record is identified as a CPU cluster, GPU, or other block.

The zone polls every 25 ms and uses ``step_wise`` with 2 C hysteresis.
After crossing the trip, the governor regulates against the lower threshold.
The original 10 C band retained near-minimum CPU clocks at 64--67 C during
interactive graphics workloads following a short initialization spike.
The user requested an 80 C threshold on 2026-09-19. Cooling now releases
below 78 C; the polling cadence is unchanged. This permits frequency
recovery between bursts of work.
Both CPU clusters have standard cpufreq cooling devices. The initial
cooling floors correspond to 2616000 kHz for the four-core P cluster and
1860000 kHz for the six-core E cluster; further cooling can reach their
minimum OPPs. Below the hysteresis threshold the governor can remove the
cooling requests. ``schedutil`` and userspace policy limits remain in force:
thermal limits are additional frequency-QoS maximum requests, not writes
that replace userspace's ``scaling_max_freq`` configuration.

Registration starts with minimum-frequency guards on both clusters. Read
failures reinstate those guards without reporting an invented temperature.
Recovery requires three valid reads and, when hot, active cooling on both
clusters. Disabling the thermal zone also retains minimum-frequency guards.
This cannot detect firmware that freezes with coherent, plausible but stale
records: the documented record ABI contains no freshness counter.

The report device must already be bound and the exact J713 CPU policy
topology and cooling-floor OPPs must exist. No other Apple board is enabled
implicitly. Because this is a board operating policy using existing devices,
loading ``apple-pmp-thermal.ko`` needs no device-tree overlay or PMP rebind.
Built-in use requires the suppliers to be initialized before this client;
module use after boot is the validated deployment path.

Unloading unregisters the zone, joins its polling work, removes the cooling
devices and this module's QoS requests, and releases its mappings/references.
It removes Linux thermal protection: unload only while idle with another
recovery path available. It must never force-unload a module or reset PMP.

An 80 C hard ceiling is not guaranteed. Sensor refresh, polling latency,
thermal inertia, workloads outside the CPUs, and minimum-OPP cooling capacity
can cause overshoot. This driver neither invents an emergency shutdown trip
nor replaces autonomous firmware protection. Independent actual-clock and
full firmware thermal-protection validation remain separate work.

2026-09-19 graphics qualification
--------------------------------

The 80 C / 2 C policy was tested in the unchanged Factorio main-menu path
with a 60-second watchdog, six sensors sampled every 0.5 seconds, and a
95 C independent cutoff. Run ``20260919T003936715681Z-factorio-thermal-80-clean-profile``
completed normally: 22.47 FPS tail, sampled peak 87.25 C. Performance-core
caps recovered to 4.104 GHz between cooling intervals; the final 30 seconds
also recorded 2.352 and 2.616 GHz caps, confirming throttling remains active.
The scene is animated and varies between launches; this is not a controlled
throughput ratio or a full thermal-policy qualification. The earlier 70 C /
2 C diagnostic reached 23 FPS briefly but its full tail averaged 19.72 FPS.
