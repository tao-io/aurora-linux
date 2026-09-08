# SuperTuxKart through Zink

`launch.py` uses the existing supervised M4 desktop's Vulkan ICD and checks
its watchdog ownership and fresh pixel-verified GPU progress. Install it as
`/usr/local/bin/m4-supertuxkart`; the packaged `supertuxkart` binary and assets
are required. Configuration is isolated under `~/.config/m4-supertuxkart`.

The initial launch selects OpenGL through Zink, a window, the basic lighting
pipeline, and uncompressed textures. No CPU renderer is selected. Actual
renderer identity and image correctness still require hardware validation.

Examples:

```
m4-supertuxkart --seconds 120
m4-supertuxkart --seconds 120 --track lighthouse --profile-seconds 60
m4-supertuxkart --stop
```

The time bound belongs to the test supervisor. It does not impose a driver
submission count or lifetime limit. The launcher remains bound to the desktop
unit so a failed health check also stops the game. Hardware validation pending.
