# J713 connected-monitor desktop module

`m4_dcpext_desktop.ko` is the explicit-load first desktop driver for the
qualified J713 DCPEXT0 / USB-C route. It registers a DRM device with live EDID,
native firmware modes, atomic modesetting and a CPU-copy primary plane. The
existing compositor discovers the card through the normal DRM hotplug event.
The normal desktop has no test-duration timeout.

Build from the workspace root (the separate MO directory is required):

```
RUSTUP_TOOLCHAIN=1.98.0 make -C build/linux-displayport-modules ARCH=arm64 LLVM=1 \
  M="$PWD/linux/linux-displayport/drivers/gpu/drm/apple/m4-dcpext" \
  MO="$PWD/build/m4-dcpext-modules" -j8 W=1 modules
```

The runtime directory retains the qualified explicit ownership/adoption path.
The tools/m4-dcpext-probe compatibility sources link here so bounded probes and
the maintained desktop use the same implementation. Desktop startup selects
the qualified native profile; runtime policy starts only after link training,
mode setup and live EDID. Bootstrap and individual RPC deadlines remain bounded.
Runtime uses an all-endpoint message-rate bound rather than a lifetime count.
Diagnostic capture storage rolls over instead of ending a running desktop.

Use tools/displayport-freeze-probe.py with --binary pointing to this build, then
`tools/displayport-host-run.py --desktop FROZEN` for guarded first activation.
The monitor must be connected to either qualified flipped pin-D port at fresh boot.
Use `--observe-only` to activate without mode changes or test clients. Activation
requires the armed 60-second SSH watchdog broker and temperature guard, tests
both Wayland outputs and mode/DPMS changes, saves evidence, and returns the
running ordinary desktop to idle watchdog supervision. No stress client remains.
Do not reload after shutdown in the same boot: coprocessor restart is unqualified.

The module is installed under the running kernel's extra/ directory.
`m4-dcpext-desktop-lifecycle.service` is active for this explicit activation and
requests orderly shutdown before the display manager stops. It does not
automatically load the module at boot.

Writing Y to /sys/module/m4_dcpext_desktop/parameters/desktop_stop requests
acknowledged shutdown. Check native_kms_worker_done and result before unloading;
userspace DRM references may keep the module loaded. Never force unload. Failed
retirement retains all uncertain DMA resources. desktop_test_ms=20000 or 30000
is reserved for bounded qualifications; the ordinary value is zero.

Hardware evidence (2026-09-17): desktop-wayland-4 passed 20 seconds; the maintained
build desktop-maintained-1 passed guarded activation with no lifetime timeout,
live Philips PHL 237E7 identity, both outputs active, 720p/1080p mode switches,
DPMS off/on and acknowledged frame completions. A later external-output Wayland
screenshot contains the Omarchy greeter. The user subsequently physically confirmed
the persistent image and HDMI-side reconnection. See workspace _provenance/displayport-v1-20260917
and experiments/20260917T094535332101Z-external-wayland-screenshot-2.

Runtime HDMI HPD now follows the locked controller cache. Removal invalidates
connector identity and powers down the sink without retiring transport/DMA.
Reconnection restores the parked crossbar before HPD-high, accepts firmware AV
service retirement/republication, and publishes only fresh native modes/EDID.
Each command/discovery wait is bounded; outstanding HPD commands cannot be
replaced by rapid cable edges. Offline sanitizer tests and guarded physical
HDMI removal/return passed with desktop-hdmi-hotplug-4 on 2026-09-17.

The first physical cycle exposed Aquamarine terminating a shared EGLDisplay,
causing Hyprland to restart in safe mode. The workspace Aquamarine fix uses
independent GBM displays when EGL reference tracking is absent; kernel hotplug
success alone does not prove compositor continuity. See the workspace handoff
for separate driver and userspace evidence.

Runtime also migrates between ATC0 and ATC1 after acknowledged HPD-low and
release, retaining the DCP route object while changing the PHY. Both directions
published fresh modes/EDID during the user's repeated cable moves. A subsequent
-ENOSPC failure identified the EDID channel's leftover no-wrap TX writer; it now
shares the maintained duplicate-header ring writer. The actual runtime header
is covered by exhaustive pointer/packet wrap tests, not only its successor.
The fixed module passed guarded dual-output activation and is installed.
Physical repeated-reconnect qualification of this fix is pending.

## TODO / qualification boundaries

- Longer desktop use and repeated hotplug qualification.
- Automatic boot loading and startup recovery. The installed lifecycle service
  requests bounded acknowledged shutdown on system stop; this service wiring
  has not yet been exercised through a reboot.
- Extended USB-C hotplug qualification and HPD IRQ handling through the normal
  Type-C provider; additional orientations, adapters and monitors.
- Replace explicit handoff/MMIO ownership with the normal platform/RTKit, PHY,
  crossbar, PMGR and IOMMU consumer integration already being developed.
- Deep sleep and display-coprocessor restart (explicitly deferred by the user).
- Audio, HDR, VRR, HDCP, MST and high-bandwidth modes are not qualified.
- Direct imported-buffer scanout / copy-performance optimization and cursor plane.
- Remove diagnostic-only options and split the retained runtime helpers further.
- Startup robustness: three earlier desktop-policy trials failed training with
  error22 before DRM publication. Original timed controls passed. Deferring the
  desktop policy until publication passed both subsequent desktop trials; this
  does not establish broad link-training reliability.
