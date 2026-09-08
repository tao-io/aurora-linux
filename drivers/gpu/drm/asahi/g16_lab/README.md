# J713 G16 dirtyroom compute laboratory

Imported from committed donor `8d31dd1170ac955d228477bb5141f028dd358399`
in `linux/m4_work_finished_kernel`. Captured dispatch resources, generated
shader binaries and the progress-marker include come from that preserved
working tree; `payloads.json` records their exact hashes and source paths.
This is a dirtyroom bring-up module containing reconstructed firmware/driver
interfaces and captured macOS resource layouts. It is specific to J713/T8132
and the recorded RTKit-3255.160.4 G16G firmware. It is not a DRM/Mesa driver.

Build externally against a matching 16 KiB arm64 kernel output directory:

```sh
make -C /path/to/linux-alpha O=/path/to/kernel-build ARCH=arm64 LLVM=1 \
  M=/path/to/linux-alpha/drivers/gpu/drm/asahi/g16_lab modules
```

The kernel needs the PMP bridge and the opt-in
`apple_j713_reserve_stage2=1` memory reservation. The stage2 device tree uses
`linux-enablement-mac,g16-lab` under `/chosen` to export GPU handoff metadata.
The default `stage=smoke` does not execute a GPU workload. Nothing autoloads
this module. Start each firmware experiment after a fresh boot: stopping the
ASC after an incomplete handshake is not a firmware reset.

Hardware validation on Omarchy J713, 2026-09-07, kernel
`7.1.9-dirtyroom-gpu1+`: the full RTKit handshake and InitData reach ready;
the fenced no-op passes after its observation-loop correction. The offline
Mandelbrot shader executes 4096 threads and produces exactly 16384 bytes:
680 black pixels, 3416 white pixels, all rows mirror-symmetric. Output SHA-256:
`792314d0bce27c82b5777d559020eba427b687d8fc4d6b1c031d9c915fc7ffc1`.
Two runs after separate boots produced the same exact output; their manifests
and GPU logs are in `validation/`. Context 5 submitted/retained generation
is 3/3, retained fault mask is zero,
all queue cursors retire, stage_result=0 and rmmod succeeds.

To reproduce on this qualified machine after a fresh boot:

```sh
sudo python3 tools/run-mandelbrot.py --module /root/g16_lab.ko
```

The runner checks the kernel/module match and exact shared-memory/firmware
segment layout before dispatch, records logs and manifests in
`/root/g16-results/`, validates GPU output, wraps it in PNG without calculating
the fractal on the CPU, and unloads the module on success. It refuses another
firmware experiment in the same boot. Failed runs keep diagnostic state for
inspection. The minimal `rtkit-initdata-ready` stage intentionally lacks fields
needed by this firmware and crashes; use the complete compute recipe above.

Every hardware experiment must first arm `/dev/watchdog0`. The runner requires
`tools/experiment_watchdog.py` alongside it, verifies the Apple watchdog and a
60-second timeout through ioctls, and persists that evidence before `insmod`.
It sends no periodic keepalives. The watchdog stays armed through submission,
diagnostics and any module removal, and is disabled explicitly only once these
return and evidence is saved. An exception leaves recovery armed. For other
bounded experiments, use `python3 tools/experiment_watchdog.py -- COMMAND ARGS`.
`--expire-test` deliberately waits for a hardware reset to validate recovery.
Allow time for firmware, GRUB and Linux to boot before retrying SSH.

The newly assembled shader body uses the donor's captured macOS dispatch
resource envelope. Regenerate its instructions with the included BSD encoder:

```sh
python3 tools/g16mandelbrot.py --output /tmp/mandelbrot.bin
cmp /tmp/mandelbrot.bin g16_mandelbrot_shader.bin
```

Shader SHA-256:
`d497442bd94c9c2862ba6f4f85b080639865a8a2d9110d892ae1d797555b6a54`.
The generator/vendor sources were copied from the local
`ESP32KeySim_m4/g16-isa` collection; the vendor README records its upstream
commit and the original BSD license is included. No macOS compiler is used by
this regeneration step. Other imported ocean/direct diagnostics remain donor
reference code and are not claimed validated on this alpha kernel.

Alpha adaptations retain the donor compute path but explicitly disable its
unused PMP tool-snapshot diagnostic. PMP power requests use the active kernel
PMP driver through a bridge serialized against driver removal.

### Native DCP coexistence: misplaced PMGR retention writes (2026-09-09)

The early GFX lab incorrectly treated PMGR `pwrgate-regs[0]` as a GFX
retention register. On the captured J713 ADT it is physical `0x38072504c`,
mask `0x00402001`, transition value `0x80000000`. The power-domain table
associates retention with DISP_CPU (domain 23, flags `0x201`); the GFX power
domain (48) has flags zero and its physical device has `pd=0`. The following
entries are the two external display CPU gates, not GPU islands.

A guarded native-display/GPU test on boot
`73305a5b-928c-4349-a174-984a38a52733` sampled DCP CPU control/status before
and 100 ms after every host GPU power operation. DCP stayed at `0x10/0x6c`
through TVM enable, the transition write, and GFX PS activation. Immediately
after the retention write its status became `0x10006c`; after 100 ms its
control/status were `0/0x1000e8`. This occurs before the AGX PMP request or
GPU firmware start. A separate AGX-only PMP request preserved DCP and was
followed by 180 verified native flips in 3.000 seconds.

Remove all accesses mislabelled as GFX retention, including startup, restore,
and reset paths. Retire the explicit retention probes with EOPNOTSUPP. Keep
GFX TVM, its actual PS register `0x380700250`, and the identified PMP AGX
request. Prior offscreen/simpledrm GPU successes did not validate coexistence
with running display firmware. The display CPU must never be reset as part
of GPU initialization. Combined rendering validation follows this correction.
