# J613 offline NVRAM write preparation

Unqualified hardware candidate; defaults read-only. No unattended hardware test.

The driver fences physical page programs and 4 KiB erases to `0x700000..0x800000`, rejects every status/configuration/lock/chip-erase command, and synthesizes real WREN only inside a range-admitted operation. Opt-in is a root runtime write to `read_only`, never a boot/module-load argument. It requires the pinned J613 DT layout; writes on other SoCs are rejected. A post-WREN fault latches read-only for the loaded module instance (no unload/reload during qualification). Existing SPI-NOR/MTD/asahi-nvram supply page splitting, partition access and bank-format handling.

Tests, all offline:

```sh
cc -O2 -Wall -Wextra -Werror tools/j613-nvram-write/policy-test.c -o /tmp/j613-qspi-policy-test
/tmp/j613-qspi-policy-test
python3 tools/j613-nvram-write/driver-mock-test.py
```

The second test extracts the actual admission/modify/setter functions afresh and runs them with a fake NOR under ASan/UBSan. It does not test MMIO or J613 wire sequencing. The policy arithmetic and fixed opcodes establish command-boundary containment; they do not establish boot-variable atomicity or resistance to controller misinterpretation.

`backup-nvram.py` is an owner-only read-to-Alba helper. `dry-run-bless.py` plus `mtd-file-tracer.c` rehearse the actual asahi-bless path on a private regular-file snapshot copy in Linux. The tracer logs offsets/lengths, never NVRAM bytes. Neither is an instruction to run tonight. `tracer-test.c` tests the shim on an all-FF synthetic private file, never MTD.

The matching candidate artifact directory is `/Users/satchlj/Projects/mac-dualboot/artifacts/j613-d3-nvram-write-20261007/`. `build-module.py`, `package.py` and `verify-offline.py` retain the exact D3h build/repack procedure. The image retains the D3h kernel release; identify the changed QSPI module via its build ID, not uname.

The authoritative qualification ladder, exact hashes, upstream/native evidence, unresolved ef6517 protection identification, owner-only RAM/install harnesses, first-write gates and Recovery limitations are in mac-aurora-omarchy:
`machines/m3-air/research/2026-10-07-d3-nvram-write/QUALIFICATION.md`.

Do not claim the same TahoeResearch value makes every mis-write benign. Do not claim bank generations guarantee iBoot fallback after every torn write. No NVRAM writes, rig changes, ESP writes, root installation or reboots were performed for this preparation.
