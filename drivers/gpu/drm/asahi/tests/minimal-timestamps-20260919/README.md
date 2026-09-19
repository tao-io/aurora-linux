# Optional G16 firmware timestamp cost

Common Asahi queue/render.rs and queue/compute.rs emits timestamp pairs only
when API queries exist. The native J713 simple render packer likewise encodes
Start/WFI/Finalize with optional records omitted. Debug bit 25 tests that form
while retaining ALL application queries and exact first/last packet brackets.
Restart offsets and WFI survive compaction; 16 host checks pass.

Full control 163336: pixels and counts exact, 5.971..6.096 ms.
The first attempt to set flags via sysfs (163352) failed before GPU work because
this Rust module exposes no writable parameter files. Flags must be changed
in /etc/modprobe.d/99-factorio-submit-summary.conf and initramfs, then reboot.
Normal flags 25165824; experimental flags 58720256. Stage helper records old
and new configuration, keeps watchdog progress enabled, delegates rollback.
Hardware qualification pending. Full CTS deferred.
