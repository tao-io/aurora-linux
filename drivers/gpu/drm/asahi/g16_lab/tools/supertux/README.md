# SuperTux on the J713 M4 lab GPU

This opt-in launcher runs Arch Linux ARM SuperTux 0.7.0-1 in the supervised
Omarchy desktop. Its OpenGL 2.1 shaders run through Zink and Honeykrisp on
T8132/G16G B1. It uses a floating 800x450 window, basic graphics, and no audio.
The private user directory is `~/.local/share/supertux2-m4`.

Start the accelerated desktop using its existing home launcher, wait for its
first GPU health check, then open `SuperTux (M4 Zink).desktop` in the home folder.
From a terminal in that desktop:

```sh
m4-supertux
m4-supertux --stop
```

The default game time limit is one hour, capped by the enclosing desktop's
remaining lifetime. `--seconds 120` selects a shorter session. `--level` accepts
an installed level file. Closing the game leaves the desktop running.
Super+Shift+Escape requests a clean exit from the accelerated desktop.
A fresh boot is required for another firmware bootstrap.

The launcher requires the existing desktop supervisor to own `/dev/watchdog0`
with its 60-second timeout and have a recent passing GPU health check. It does
not renew the watchdog itself. The supervisor verifies real Vulkan pixels and
compositor responsiveness before renewing; a stalled GPU is allowed to reset.
The game runs as the desktop account, with no process capabilities.

`glcheck.c` observes the actual current SDL context and logs `GL_RENDERER` and
`GL_VERSION` to `journalctl -u m4-supertux.service`. It rejects a software or
non-M4 renderer. It does not implement GL or alter shader outputs. The observed
renderer is `zink Vulkan 1.4(Apple M4 (G16G B1) (MESA_HONEYKRISP))`.

## Private SDL dependency

The system SDL2 compatibility library leaves `GL_INVALID_ENUM` in an OpenGL 2.1
context by querying `GL_MAJOR_VERSION`. SuperTux detects that stale error and
aborts. Classic SDL2 2.32.10 contained the same query. The included patch uses
`glGetString(GL_VERSION)` for this version check instead, without clearing
unrelated GL errors or overriding the advertised GL version.

The private source is SDL's `release-2.32.10`, original commit
`5d249570393f7a37e037abf22cd6012a4cc56a71`, with the included patch applied.
The local checkout is `reference/sdl2-2.32.10`, commit `d22d7d9`.
Build it for aarch64 with Wayland and shared-library support, without X11,
KMSDRM or tests. The workspace build uses the existing graphics sysroot and
`/home/council/omarchy/graphics-build/aarch64-toolchain.cmake`:

```sh
cmake -S reference/sdl2-2.32.10 -B build/sdl2-m4 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/home/council/omarchy/graphics-build/aarch64-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
  -DSDL_WAYLAND=ON -DSDL_X11=OFF -DSDL_KMSDRM=OFF \
  -DCMAKE_INSTALL_PREFIX=/opt/m4mesa/supertux
cmake --build build/sdl2-m4 --parallel
```

For this cross-build, set `PKG_CONFIG_SYSROOT_DIR` to the graphics sysroot and
`PKG_CONFIG_LIBDIR` to its `usr/lib/pkgconfig:usr/share/pkgconfig` directories.
Install the built `libSDL2-2.0.so.0.3200.10` as
`/opt/m4mesa/supertux/lib/libSDL2-2.0.so.0` on the target. The launcher prepends
this private directory only for the game; system SDL packages remain installed.

Build the observer with an aarch64 compiler:

```sh
aarch64-linux-gnu-gcc -Wall -Wextra -Werror -O2 -shared -fPIC glcheck.c \
  -o m4-supertux-glcheck.so -ldl
```

Install the observer in `/opt/m4mesa/lib`, `launch.py` as executable
`/usr/local/bin/m4-supertux`, and the desktop file as an executable owned by the
desktop user in their home folder. SuperTux itself is the packaged
`/usr/bin/supertux2`; its packaged data files are unchanged.

## Validation boundary

Framebuffer captures and user observation established correct level rendering,
sprites, transparency and movement. The initial playable level ran at roughly
5–6 FPS; a later capture after reducing completion polling latency showed
7.7 FPS average. This is experimental acceleration, not a full Vulkan conformance or
performance claim. Quickshell still uses Qt software rendering before GPU
composition, as documented by the Hyprland launcher.

Sustained gameplay exposed a compute-bootstrap command alias inside the USC
freelist: after roughly a minute, recycled page addresses overwrote the UMA
descriptor. Rendering now owns corrected low USC page tables; the descriptor
check fails closed if its hardware ID changes. See the kernel commit and the
workspace's `supertux-20260908` hardware evidence for the test results.

The corrected mapping completed a 200-second SuperTux level session with
movement/jump input, 4,966 completed GPU jobs at game exit, and continuing
independent Vulkan pixel checks. The evidence run is
`20260908T135007Z-render-ta`, boot
`6a3a2023-a1e0-46bc-aeb2-c515e9ba9d07`, module SHA-256
`d90f5731e58e171c63e05fd179f7dde29382930a9630f5bf137c07417d24aa38`.
The helper's `running-90s/135s/180s/225s` filenames are legacy labels;
actual intervals were 40 seconds, and the full game lifetime was 200 seconds.
This establishes a bounded run beyond the earlier repeated failure, not
indefinite stability.

The enclosing 300-second desktop run then passed in full: 5,098 client VM
publications/completed jobs, 55 successful GPU health checks, clean module
removal, watchdog disarm, and no new kernel fault. Its final manifest reports
`hardware_regression_passed: true` and `passed: true`.


## Performance and retirement checkpoint, 2026-09-08

The M4 lab memory preference now places GPU-heavy allocations in WC memory,
while retaining a separate CPU-cached memory type. The kernel maintains the
union of cached CPU ranges, merging executable aliases without merging gaps.
A five-second worker profile changed from about 67% CPU cache maintenance to
less than 1%; remaining samples predominantly wait for GPU/firmware progress.
This is real GPU shader execution with CPU driver overhead.

Faster submissions exposed two command-lifetime bugs. Firmware ring retirement
alone was insufficient: its deferred notifier still referenced command metadata.
Both stages now wait for their separate firmware stamps as well as GPU stamps
and queue retirement. Each stage publishes the cumulative host work count to
the shared notifier target; without it, firmware could defer notification until
later commands that a synchronous submitter cannot issue.

Run `20260908T143717Z-render-ta`, boot
`ae94dabb-aa14-429f-b76d-bfbf6dc47085`, passed with 29,759 completed jobs,
a 240-second SuperTux session, independent Vulkan pixel checks, clean module
removal and watchdog disarm. Module SHA-256:
`5f3ca3884286afb9125cd1a197948986e05642daf1197bf82204b8659128ae42`.
The first-level screenshots show correct rendering and movement. Warm gameplay
swap cadence was roughly 24–37 FPS, including slower windows; this is not a
60 FPS result or a measurement of physical display presentation.

That run also enabled `--render-power-controller`, using the existing qualified
J713/25G83 four-object power configuration. Its firmware telemetry remained at
pstate 1 with performance limit 100. Clock scaling is still under investigation.
The earlier optimized runs `20260908T141646Z` and `20260908T142246Z` failed at
1,513 and 8,020 jobs respectively; they are diagnostic failures, not sustained
passes. Complete artifacts are in the workspace under
`_provenance/omarchy-m4-render/hardware/supertux-speed-20260908/`.

The per-job VM table snapshot reduced CPU submission preparation from roughly
3–7 ms to 0.2–0.7 ms and reached 30–40 FPS windows, but exposed failures. GEM
close invalidates the cached VM tree; rebuilding it lazily fixed missing command
leaves. Later health-check draws still faulted at VA `0x40`, with valid command
mappings, after 207 and 778 jobs (`20260908T150506Z` / `20260908T151212Z`). Full
outer-shareable TLBI did not fix that fault and remains opt-in.

Run `20260908T152033Z-render-ta` used the original page-by-page builder as a
control, plus the Mesa instruction-encoding optimization. It passed 22,980 jobs,
56 independent GPU health checks, a 240-second SuperTux session, clean module
removal and watchdog disarm. Gameplay was roughly 20–30 FPS. This isolates an
optimization regression; it is not the requested 60 FPS result. A bounded
pre-publication delay is available to separate timing from table semantics.

### Mandatory temperature monitoring

The render runner now requires `experiment_thermals.py` alongside
`experiment_watchdog.py`. Before loading the GPU module it requires one
`apple_pmp` hwmon provider, the six qualified labels, valid readings below 65 C,
and enabled `apple-pmp-die` CPU thermal policy. It logs all readings and CPU
frequency caps to `temperatures.jsonl` every 0.5 seconds, and records per-sensor
peaks in the manifest (or `thermal-summary.json` on failure).

At 75 C, a failed sensor read, or disabled CPU thermal policy, the monitor
latches failure, stops SuperTux, requests compositor shutdown and stops the GPU
submission service. Failure or a monitor older than two seconds prevents further
GPU-verified watchdog renewals. The existing watchdog remains responsible for a
wedged GPU. The monitor never feeds it itself and never changes clocks.

These are conservative experiment limits, not qualified hardware safety limits.
The PMP identifiers do not establish sensor placement or a dedicated GPU sensor.
CPU thermal throttling remains enabled; GPU thermal throttling is not implemented.
Do not raise GPU clocks under this limitation. The earlier runs above predate
this monitor and must not be presented as temperature-qualified stress tests.

### Faster submission and allocation cache transfer

The monitored 3 ms delay control `20260908T153146Z-render-ta` passed 25,789 jobs
and 54 independent health checks; its hottest recorded channel reached 69.25 C.
`20260908T154020Z-render-ta` removed that delay after entering the level and
passed 35,347 jobs and 69 checks, with warm 35–44 FPS windows and a 72.25 C peak.
That intermittent fault therefore cannot be explained by the table snapshot
alone; these passes do not erase the earlier cold-start failures.

The lab's direct physical UAT mappings bypass the DMA API's initial cache
transfer. ARM64 shmem helpers can therefore leave dirty page-initialization
data in the cached linear alias before exposing a WC mapping. Native WC GEM
creation now pins its pages and cleans/invalidates that alias once, before
publishing the handle. The pin survives until object destruction. No new
per-submission cache sweep is added.

Cold-start run `20260908T154928Z-render-ta` used that fix with no diagnostic
delay from startup. It passed 34,524 jobs, 63 independent pixel checks, a
240-second game session, clean module removal and watchdog disarm. Module
SHA-256: `02ce57e0fd181e23e55210ec11a1e4cd7c13085a33ff237105f2241796e0d3bf`.
Warm windows reached roughly 40–44 FPS; the hottest individual reading was
68.5 C. This is a bounded regression pass, not a 60 FPS result or indefinite
stability proof.

Routine successful stage/event/statistics messages now stop after the first
eight frames unless `render_trace_submissions=1`; failures, recovery events and
128-job timing summaries remain. Validation uses `client_jobs_completed` from
the stopped worker, while `client_vm_publications` only counts retained log
lines. Mesa's routine VDM logs require `HK_G17P_LAUNCH_TRACE=1`. An indexed Ioto
decoder retains all prefix/ambiguity checks and passed 177,308 host checks;
the combined hardware performance comparison follows these changes.


### Polling, CPU scheduling and scanout profiling

`20260908T155717Z-render-ta` passed 33,788 jobs and 80 pixel checks, with a
69.75 C peak. Sleeping between active GPU polls then reduced the render
worker from about 43% to 6% of one CPU in five-second samples. The monitored
`20260908T160855Z-render-ta` passed 34,405 jobs with a 69.5 C peak. Its normal
warm windows remained roughly 30–43 FPS. A recorded 20-second CPU performance
governor control reached 48–51 FPS; CPU thermal caps remained active, and the
original schedutil governors were restored afterward. GPU clocks stayed at
pstate 1 (338 MHz). This was not a sustained 60 FPS result.

Telemetry still samples every 0.5 seconds; disk synchronization is batched at
five seconds and at shutdown. A thermal failure requests workload stop before
attempting to persist the failure report.

`20260908T161554Z-render-ta` failed during an independent health submission
with a context-6 level-2 read fault at VA `0xbfff9eaa80` (unit `0x21`). Its
highest reported temperature was 66.75 C; this was not a thermal cutoff. The
watchdog reset the machine. Recovery failures now dump the active command and
verify the job's UAT leaves against pinned GEM pages before teardown, just as
timeout failures already did. The failure remains unresolved; later bounded
passes do not establish indefinite stability.

Mesa now binary-searches the generated AGX3 form tables, with every known
mnemonic and all 256 ep values checked against the generic first-match search.
All 1,494 captured instructions still encode byte-for-byte. M4 graphics Ioto
programs also share 128 KiB executable allocations within each command buffer;
individual aligned program ranges and their allocation lifetime are retained
through command-buffer reset. `20260908T162433Z-render-ta` passed 33,580 jobs
and a 240-second SuperTux run, peaking at 69 C. Neither change alone produced a
clear FPS gain.

Sampling all kernel worker threads identified the final CPU scanout conversion
as a substantial remaining cost: 2,491 samples in two workers, predominantly
memcpy and `drm_fb_xrgb8888_to_xrgb2101010_line`. Simpledrm maps its framebuffer
with WC attributes; on ARM64 this is Normal-NC memory. The new optional
`drm_sysfb_helper.neon_blit` path converts that memory in one vector pass,
retaining the generic conversion when disabled or unsupported. SIMD ownership
is bounded to one row. The exact assembly passes exhaustive 24-bit RGB,
unaligned/tail and guard-page tests under qemu-aarch64; hardware qualification
follows. Run the standalone test with:

```sh
python3 drivers/gpu/drm/asahi/g16_lab/tools/hyprland/test_sysfb_neon.py
```


The first SIMD scanout boot used Image SHA-256
`a7ba977047fb1a1b10785af25a62e7d751374b55da50937bb70cec8337e02336`.
Run `20260908T163717Z-render-ta` was stopped deliberately after the user's 1TR
report, pending clarification of target identity. The SSH target remained on
boot `e7788467-4ca4-45e3-94af-7175b2459fd6`, with the native SMC reboot driver
bound. It completed 11,267 jobs and 24 independent pixel checks, removed the
module cleanly, and disarmed its watchdog after evidence collection. The
hottest recorded channel reached 70 C without a thermal cutoff. This was an
abbreviated run, not the planned full gameplay comparison. FPS did not show
a clear improvement; runtime A/B profiling of the SIMD path remains pending.
