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

First minimal boot rejected retirement at desktop job42: exact firmware/user
stamps b00, all queue/pipe indices drained, but omitted compute timestamps zero.
The host requires that pair as extra compute retirement proof. Preserve the
requirement and retain ALL compute timestamp pairs; scope this experiment to
unrequested render-engine pairs. Sealed fault retained in provenance163800.
The decoder now accepts both explicitly known 4KiB/8KiB compute command sizes.
