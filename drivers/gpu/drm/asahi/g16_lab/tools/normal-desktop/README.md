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
