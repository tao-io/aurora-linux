# LG 5K hardware checks

Run `python3 -m unittest discover -s tools/aurora-display -p 'test_*.py'` to check the tools.

Run `display_probe.py` as the desktop user on the test Mac. It reads Thunderbolt, EDID, DRM and Hyprland state and emits TOML. `--require-mode` returns 1 when the software 5K60 checks fail. Scale does not change the required physical mode. A software pass does not prove pixels on the panel or live tile routing.

Run `boot_backup.py --backup` with sudo on the J313 M1 Air. Both `/boot` and `/boot/efi` must be mounted. The tool copies the full boot tree, checks each copied file and writes a root-only archive and receipt below `/var/lib/aurora-display/backups`. It does not change boot files, select another kernel, reboot or test recovery. Keep the old kernel, initramfs and early boot bundle together.

The hardware baseline has Thunderbolt authorization but no external DRM connector or EDID. Get a live routing readout and observe a full-panel test pattern before marking support as verified.
