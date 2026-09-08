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

### Desktop stop at 65,535 jobs (2026-09-09)

Run `20260908T222849Z-render-ta`, boot
`89c52123-c6c2-489c-91a6-723edd752ae6`, stopped firmware normally at uptime
612.343 s after about 535 s of rendering. Submissions then returned ESHUTDOWN,
the next independent GPU pixel check timed out, and the supervisor closed the
desktop. The service loop reached its configured 65,535-job limit despite a
3,620-second lifetime. The continuous log has 511 full 128-job timing batches
before its final partial batch, no GPU fault, and an orderly firmware teardown.
101 health checks had passed. All six temperatures remained below 63.75 C.
The later watchdog recovery follows the lost health check, not a thermal trip.

The first correction used the full nonzero 24-bit job-index range
(0xffffff), which fits the 32-bit firmware stamp `sequence << 8`. The elapsed
service deadline and per-job waits remain bounded, as do the watchdog and
thermal guards. Small explicit fixture budgets are preserved. Service exit
logs now record the completed count, budget, result and stop request so an
intentional limit is distinguishable from a hardware fault. Validation must
cross the old 65,535-job boundary before calling that correction tested.

The user requested removing lifetime cutoffs entirely rather than raising them.
DRM service now runs until an explicit client stop, thread shutdown, or actual
hardware error. `render_frames` and `render_service_seconds` only bound fixture
experiments; the userspace supervisor retains test duration, thermal checks and
watchdog ownership. Client tests always send an explicit stop when finished.
Host submission accounting is 64-bit. Hardware completion stamps cycle through
all nonzero 24-bit indices with the low eight bits clear, so crossing either
65,535 or the stamp rollover cannot accidentally match a cleared completion
word. `test_sequence.c` checks both boundaries and a complete stamp cycle under
UBSan. These are wire-format constraints, not a job-count service cutoff.

The serial regression `20260908T224300Z-render-ta`, boot
`f72f966f-9bb8-46f5-80a3-1a9f31c3c8bf`, completed 85,008 jobs and 134
independent eight-frame Vulkan health checks, then stopped explicitly and
unloaded successfully. No new kernel fault occurred. The six-channel peak was
69.5 C; the watchdog was disarmed only after evidence collection. This run used
the initial enlarged budget; the no-cutoff source and wire rollover are separate
changes and still require their own deployed validation.

### Asynchronous submission

`render_async_submit=1` enables ordered CPU-side submission queueing. IOCTLs
validate and snapshot the VM, pin GEM objects and CPU mappings independently,
then publish output fences and return. One worker waits on input/implicit
fences and feeds the existing single active hardware job. It retains the
snapshot until retirement or firmware teardown. Queue capacity provides
backpressure on outstanding allocations, without limiting total jobs or uptime.
The serial path remains available for A/B diagnostics. This overlaps client CPU
work with GPU execution; it does not yet overlap TA and fragment hardware jobs.

`M4_SUBMIT_ASYNC_LIFETIME=1 g16submittest ...` additionally queues 16 identical
final draws, closes every GEM handle, destroys the queue and VM, unmaps DRM
VMAs and closes the DRM file before waiting for its still-pending exported
fence. Pixel readback uses a separately mapped dma-buf. The ordinary test also
waits for completion before querying timeline progress, as required by async
submission semantics. Build and host compilation pass; hardware results follow.

Hardware validation `20260908T225921Z-render-ta`, boot
`8977f424-c7dc-4b0e-a729-2bd784b424c4`, passed all 24 async jobs and
524,288 pixel comparisons. The final exported fence was still pending after
all client handles, VM, queue, DRM VMAs and the DRM file were closed. Binary
fences and completed timeline points passed, as did module unloading with no
new kernel fault. Both startup and GPU execution used the 60-second watchdog
and six-channel thermal guard. The initial test attempt stopped before GPU
submission because its dma-buf export omitted DRM_RDWR; this test-only bug was
corrected before the passing run.

Async native desktop validation remains incomplete. Runs
`20260908T230059Z-render-ta` and `20260908T230453Z-render-ta` faulted in TA
at VA 0x40, with all retained client mappings verifying correctly. Adding a
submitting-CPU DMA write barrier did not cure this. A full-TLBI control run,
`20260908T230723Z-render-ta`, reached 6,863 jobs and approximately 60 FPS in
SuperTux before another fault (VA 0x12380). Full invalidation is therefore not
a demonstrated fix. The deployed desktop was returned to serial submission.

Whole-VM cache invalidation at completion is unsafe for asynchronous clients:
another CPU can be preparing a future buffer in the same VM. A separately
installed Mesa `ASAHI_M4_WC_ONLY=1` diagnostic removes cached Vulkan memory
and requests WC internal BOs, to test this concrete coherency hazard. This
is pending hardware validation, not a confirmed explanation of either fault.

The WC-only run `20260908T231605Z-render-ta` also faulted after 5,022 jobs
(VA 0, unit 0x41), despite zero client cache-maintenance bytes. Combining WC
with full TLBI in `20260908T232125Z-render-ta` faulted after 1,263 jobs
(VA 0x40, unit 0x21). Neither cache diagnostic is a fix for the async fault.
The latter run retained the complete submitted render packet, all private UAT
pages and up to 128 KiB from each bound client range before releasing pins.
`run-render.py` saves these under `drm-fault-state/` on failure. Filenames
record VA, complete mapping size and PTE flags; data can be a truncated prefix.
The captured health-check root descriptor has a valid self-pointer and vertex
buffer address; further GPU execution-state analysis is required.
