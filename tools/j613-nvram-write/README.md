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

Evidence-pass update: the controller platform device now exposes `flash_status`
(read-only sysfs). It sends only fixed RDID/RDSR1/RDSR2/RDSR3 reads through
spi-mem serialization, with no input or store callback. It returns raw register
bytes without claiming an exact ef6517 status-bit interpretation. Samples are
sequential. Read it only in the later owner-supervised candidate RAM window;
the running D3h does not expose this file and must not be reloaded tonight.

25G83 static analysis corrected TX-only descriptors: shared/multi count zero,
word 1 includes opcode, address and payload. WREN/WRDI use count 1, SE count 4,
PP count 4+payload length. The D3h RX header/data split remains unchanged.
`python3 tools/j613-nvram-write/descriptor-test.py` compiles the actual descriptor
block and checks it against native PIO samples; this is not a wire test.

Torn-write recovery is not guaranteed. Published XNU appends before invalidating;
the pinned asahi-nvram v3 writer serializes old invalidation before replacement.
Rotation writes a newer bank header before its complete payload; no bank-wide
checksum or commit marker establishes payload completion. Qualification must
review the actual private-file trace before any physical mutation.


2026-10-08 packaging correction: the exact D3h `M=` build reproducibly emits
an unusable distilled `.BTF.base` (ambiguous `device`). Shipping uses the
explicit no-BTF fallback, removing `.BTF` and `.BTF.base`; `build-module.py`
records the transformation tool version. GNU objcopy 2.46.0 reproduces the
shipped module already loaded read-only on D3h. This is a BTF bypass, not a
claim that module BTF validates. See the qualification receipt for exact hashes.

`package.py` verifies identical module bytes in the root payload and initramfs,
and retains the original D3h rollback module. `install-root.py` remains plan-only
unless explicitly applied in an owner window. Root installation must precede
RAM qualification because switch-root may load the root copy. Its extra apply
flag is `J613_CANDIDATE_ROOT_INSTALL_APPROVED=1`; the other owner/rig/haptics
flags still apply. The bundled `check-d3h-root.py` includes the Mesa wrapper
exception, and uses the installed original D3h manifest. Both baseline and
candidate root checks preserve all other D3h expectations. ESP apply requires
the same candidate root hash plus a matching RAM receipt. No install or reboot
was performed for this packaging correction.

`ram-observe.py` now requires the candidate root hash before boot and waits for
parameter mode 0644 and the exact loaded module note, never `test -w`.

`dry-run-bless.py` requires private paths, uses independent regular-file copies,
and runs installed asahi-bless inside bwrap mount/PID/network namespaces with
MTD nodes hidden and capabilities dropped. The only exposed device nodes are
the inventory NVMe nodes used read-only by upstream; the preload shim denies
writable device opens and writes. Synthetic tracer tests exercise this denial.
All tool stdout/stderr stays private; only TRACE metadata is exported.
`summarize-dry-run.py` compares v3 snapshots privately and emits changed-key
names, offsets, counts, generation deltas, and byte/page ranges without values.
The refreshed current-baseline rehearsal succeeded, with a one-page append and
no erase or generation change. This does not qualify physical NOR programming
or power-loss recovery.
