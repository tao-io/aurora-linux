# SuperTuxKart through Zink

`launch.py` uses the existing supervised M4 desktop's Vulkan ICD and checks
its watchdog ownership and fresh pixel-verified GPU progress. Install it as
`/usr/local/bin/m4-supertuxkart`; the packaged `supertuxkart` binary and assets
are required. Configuration is isolated under `~/.config/m4-supertuxkart`.

The initial launch selects OpenGL through Zink, a window, the basic lighting
pipeline, and uncompressed textures. No CPU renderer is selected. Actual
renderer identity was confirmed on J713; image correctness remains unqualified.

Examples:

```
m4-supertuxkart --seconds 120
m4-supertuxkart --seconds 120 --track lighthouse --profile-seconds 60
m4-supertuxkart --stop
```

The time bound belongs to the test supervisor. It does not impose a driver
submission count or lifetime limit. The launcher remains bound to the desktop
unit so a failed health check also stops the game.

## Checkpoint, 2026-09-09

The packaged SuperTuxKart 1.5 selects Zink on Apple M4 (G16G B1), in a native
Wayland 800x450 window. Startup first exhausted Mesa's 24-MiB VDM alias heap,
then the kernel's 8,192-object cap. Those allocation restrictions have been
removed; see `render/ALLOCATION_SCALE.md` for the memory regression.

With the allocation fixes and temporary 1.8/1.86-GHz CPU caps, startup gets
through shader setup to GUI skin loading, then the AGX3 compiler rejects this
fragment instruction:

```
$r3l = convert #11, ^r3, #0x1
```

The refusal is at `agx3_pack_binary.c:6210` in the Mesa diagnostic build based
on `602234d48e4` (diagnostics committed as `a1f7dabced3`). Zink receives
`VK_ERROR_FEATURE_NOT_PRESENT` and the game aborts. The captured game window
is black: no successful race or correct SuperTuxKart rendering is claimed.
Hyprland and its verified Vulkan health checks continue after this game exit.
Evidence journal invocation: `cc2e8dfb17e5401b9b6a580403fb94e1`, desktop run
`20260908T234849Z-render-ta`, J713 boot `d7192acf-2ba4-4f98-aad2-0abdd55b137c`.

A five-second CPU sample during startup contained 2,713 samples in
`g16_render_uat_leaf`, 1,711 in `vmalloc_to_page`, and 232 in
`g16_drm_vm_bind_one`. Rebuilding the complete UAT on every bind is a measured
startup bottleneck. Incremental or deferred table maintenance is future work;
it must preserve rollback and submitted-job ownership. The earlier async GPU
fault also remains unresolved; async is disabled in the recovered desktop.

The test launcher restricts game CPU time to 150% of one CPU. The native
desktop guard caps CPU frequencies and stops at 75 C; GPU clocks remain at
338 MHz. At this checkpoint the game is stopped and the desktop is healthy
at approximately 53 C. Its existing one-hour supervised session remains active.
