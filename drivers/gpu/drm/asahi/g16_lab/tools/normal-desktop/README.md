# Normal J713 accelerated desktop

These files configure ordinary SDDM and the full user's Omarchy configuration
with the installed Asahi/Honeykrisp/Zink and native DCP stack. Stage the files at
`/root/m4-normal-desktop-stage` and run `install-normal-desktop.py --backup NEW_DIR`
on J713. It saves replaced configuration, installs the supplied DCP module, and
enables SDDM. Private Mesa and Hyprland builds must already be installed.
The active EFI GRUB default must select `graphical.target`.

The graphics service discovers native DCP instead of assuming a fixed DRM card
number. SDDM's explicit `GreeterEnvironment` is necessary because it sanitizes
the helper environment. User-session wrappers load the full Omarchy Lua config.
No effects are disabled here. Existing experimental driver options are explicit
in graphics.env; this is development integration, not a conformance claim.

`normal-login-check.py --output NEW_DIR --existing-greeter --native-screenshots`
reads the login password from stdin and tests actual SDDM authentication,
Hyprland/Quickshell GPU libraries, panel/background layers, and launching Foot.
It requires installed watchdog/thermal helpers in `/root/g16-submit-test` and
uses watchdog60 plus the six-sensor 0.5-second thermal policy. Test CPU caps are
restored on success. It saves screenshots and logs before disarming; uncertain
failures preserve watchdog recovery. Visually inspect the screenshots.

`--capture-compiler` enables failure capture for sessions launched during that
test. The flag in /run is removed afterward. The compiler capture/replay tools
live in Mesa. A greeter already running before the flag was created retains its
existing environment. No passwords are saved by these tools.

Native snapshots use the read-only DCP `m4_scanout` debugfs interface; convert
saved files on the host with `scanout-image.py INPUT.m4fb OUTPUT.png`. They avoid
issuing additional GPU readback draws while testing ordinary login.

Workspace development copies are under tools/normal-desktop and tools/.
Keep these tracked copies synchronized when making reusable changes.

## Normal login qualified on J713, 2026-09-12

Mesa `ba9d84f8bc1` fixes premature pixel-ordering release in an early
discard-only coverage update. The full captured 177-draw Hyprland prefix now
completes with normal batching; its 4,259,840 pixels match llvmpipe within one
RGB level and identical alpha. No synchronized-draw diagnostic or disabled
border is needed. The installed Vulkan library SHA256 is
`099d10426e143d95cf5ca9578c2e3e1138cf30c71df74c3d7c8e0415e5f2fb94`.

Real password entry passed both a display-manager start and the greeter from
normal boot. Native screenshots show Quickshell's panel/background and a bordered
Foot window; Hyprland/Quickshell map private M4 Vulkan. Evidence directories in
the workspace are `_provenance/normal-login-20260912/` followed by
`m4-normal-final-coverage-20260912/` and `m4-normal-boot-coverage-20260912/`.
These are bounded startup/rendering results, not long-duration stability or
conformance qualification. Keep the watchdog and thermal guards for experiments.

## Ordinary SuperTuxKart startup diagnostic

The login check now pastes `/usr/bin/supertuxkart` into Foot using the normal
clipboard shortcut. Per-key input lost characters in long commands; a native
snapshot is saved before Return. The real user game settings are retained.
`--trace-stk` captures EGL calls and native shader/launch diagnostics under
`~/.local/state/m4-login-tests/RUN/`. `--trace-hyprland` enables the installed
trace wrapper for this test only, saving `process/timestamp traces in ~/.cache/m4-login-traces/`.
The runtime marker is removed on completion. A live kernel log stops input on
GPU faults; failed experiments save evidence and request recovery while keeping
the watchdog open. Startup images need visual review; this does not qualify
menu navigation or a completed race.

Subsequent ordinary-use tests found vertex-engine faults in both SuperTuxKart
(128x128 render, job6114) and Hyprland (2560x1664 render, job242). The successful
login captures above do not establish that these workloads are reliable.

`--login-only` leaves login and Foot ready for a separately guarded GUI test.
`desktop-ui.py serve GUARD_OUTPUT --seconds 300`, launched under the existing
`gpu-experiment.py` guard with a longer outer bound, accepts screenshot, clipboard
paste and held-key requests over its local socket. Paste saves a screenshot
before Enter; use the `key` action for a 150 ms hold so game frames observe input.
For example, `desktop-ui.py send GUARD_OUTPUT '{"action":"key","key":"Return"}'`.
Review each native snapshot before the next menu transition. `finish` closes the
capture and terminates newly started SuperTuxKart processes. A successful capture
session is not a successful game test: menus/race screenshots and application
logs must establish that independently. The helper requires the ancestor guard's
watchdog60, fresh GPU retirement and six-channel thermal evidence.

Real GUI Singleplayer tests reached the menu but aborted while compiling the
kart preview. Captured shaders identify nonzero floating branch constants,
packed R11G11B10 blend accesses, and F16 EOT output as separate compiler gaps.
No completed race is established by those captures.

The host-side `desktop-ui-remote.py REMOTE_OUTPUT JSON LOCAL_IMAGE_STEM` wraps
that single request, fetches the named `.m4fb`, and runs the adjacent
`scanout-image.py` to produce a PNG. It launches no experiment on its own;
the server must already have a live guard. This reduces repeated SSH/quoting
and snapshot-copy commands during real menu navigation.
