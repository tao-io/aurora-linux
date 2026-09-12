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
trace wrapper for this test only, saving `~/.cache/m4-login-traces/hyprland-PID.trace`.
The runtime marker is removed on completion. A live kernel log stops input on
GPU faults; failed experiments save evidence and request recovery while keeping
the watchdog open. Startup images need visual review; this does not qualify
menu navigation or a completed race.

Subsequent ordinary-use tests found vertex-engine faults in both SuperTuxKart
(128x128 render, job6114) and Hyprland (2560x1664 render, job242). The successful
login captures above do not establish that these workloads are reliable.
