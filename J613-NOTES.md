
## D3 haptics transport packaging regression (2026-10-07)

The published D3 branch already contains all fourteen haptics patches. The plain
25G83 source copy used to build `7.1.9-eryk-m4j613-j613-d3` instead omitted 0001,
0002 and 0003. The built-in dockchannel SET_REPORT forced HID_OUTPUT_REPORT, so
actuator feature 0x21 stayed `21 00` while host click synthesis was enabled.
The firmware and host both generated clicks. Pressure thresholds were correct.

The dedicated build tree now has those three patches, with 0004–0014 preserved;
magicmouse and actuator sources match daily #37 byte for byte. D3h uses the same
config and `LOCALVERSION=-j613-d3h`. Run
`python3 tools/j613-haptics-source-check.py /path/to/build/source` before building
archive-derived trees. The branch's transport sources match the repaired build.

The combined off-ESP candidate preserves the installed parity loader/DT fixes,
adds the prepared forced-read-only QSPI/NVRAM changes, and replaces kernel and
all boot/root modules coherently. Hardware qualification is owner-controlled:
RAM boot, measured finger pressure with feature `21 01`, zero 0x40 errors,
multitouch ready, 2560x1664 with notch, read-only NVRAM checks, then ESP install
and installed boot. No QSPI write qualification or #37 selection is included.
