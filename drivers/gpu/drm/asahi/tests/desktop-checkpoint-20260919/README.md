# Normal desktop checkpoint, 2026-09-19

User ended the optimization session and requested builds, cleanup, commits,
pushes, and a usable unified kernel boot. Full Image/modules/device trees,
clean Mesa Vulkan library, and m1n1 release builds passed. The wire suite has
16 passing tests and the native CDM codec has 3971 passing checks.

GPU support and USB-C DisplayPort are included in the unified kernel. The
existing boot menu, recovery entries, graphical target and e autologin remain.
The matching Asahi module is in initramfs; the DCP helper remains installed.
The immutable firmware-cache and compute-stamp batching probes default OFF.
The successful wide CDM visibility/barrier batching remains enabled.

The bootloader performance series is preserved in reference/m1n1 because its
local checkout has no remote. Installed bootloader already includes cache and
memory/fabric initialization; it was not replaced during this final cleanup.

Current exact five-frame replay: roughly6.0ms whole GPU submission, pixels
and all query counts exact. Last bounded live menu observation: median59.97FPS,
mean58.799FPS with dips. Neither perfect60FPS nor the5ms target is claimed.

Pre-existing, unrelated Mesa Buoy/desktop tooling was preserved in local stash
f1c4ce7f2a8a28ba24cc36ac6e88e7b4c76f8e92, plus a workspace provenance patch.
It is excluded from the clean final Mesa build and optimization push.

TODO:5ms complete-frame target, broader Vulkan CTS validation, reusable
firmware-private allocation/cache maintenance if warranted, existing GPU
thermal-throttling/deep-sleep work. No additional optimizations are in flight.

Final boot ea1d88ed-e8bd-49ab-afc6-a55241769989 runs build #20, with the
recorded Image and clean Mesa hashes. SDDM, m4-graphics, SSH watchdog and the
normal uid1000 Hyprland session are active. eDP-1 is enabled2560x1664@60Hz,
DPMS on, hardware cursor active. GPU jobs continue retiring. No external
connector is currently detected; the built-in DCPEXT driver awaits attachment.

Boot logs retain existing bring-up errors, including a USB-core initialization
timeout and early UAT handoff/page-ownership diagnostics. This checkpoint
establishes a running desktop, not a clean-kernel-log or full hardware claim.
The first observer accidentally used a nonexistent private hyprctl path;
boot.json records correction to /usr/bin/hyprctl and the successful query.
Temporary private experiment ICDs/libraries and staging copies were removed;
rollback kernel/initramfs/Mesa files and measurement evidence remain.
