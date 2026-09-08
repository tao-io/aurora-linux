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
