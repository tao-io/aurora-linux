# J713 connected-monitor desktop module

The canonical source is now `aurora-silicon/dirtyroom-linux`, branch
`eryk_m4_gpu`, shared with the GPU and internal-display work. The former
`m4-displayport` branch is retained as a historical checkpoint. Continue kernel
changes in workspace `linux/linux-asahi` (`m4-asahi-port` locally), and push
explicitly to `dirtyroom HEAD:refs/heads/eryk_m4_gpu`.

The unified branch preserves both parents: the DisplayPort implementation and
the main kernel's GPU shmem reclamation, compute-depth policy, timing/thermal
work and D114 callback fixes. The boot qualification below supersedes the earlier paused checkpoint state.

## Unified kernel boot qualification (2026-09-17)

Merge `e1eb8648d446` combines every committed change from main tip
`ff8c18c0a35d` and DisplayPort tip `c47df0457a69`; ancestry checks confirm
neither parent has omitted commits. Canonical remote: `dirtyroom/eryk_m4_gpu`.
Build source: `linux/linux-asahi`; Image, J713 DTB and the explicit DCPEXT module
were built against `build/linux-asahi-j713`. The old DisplayPort worktree's
three pre-existing uncommitted drafts remain preserved, outside this merge.

The M4 is now running this combined kernel (#15), boot
`8b26c6d9-acd2-4ed3-abe3-407ac1d4afe9`. Guarded ordinary Wayland activation
passed with eDP-1 at2560x1664 and DP-1 at1920x1080. Peak temperature59.25 C;
60-second watchdog returned to armed/idle. Display manager and orderly external
display lifecycle services are active. The matching module is installed.

Image SHA256: 99213bac8427d43e348922e44dd697e5758ad0475a825170f9c967d3fbca7e1b.
Desktop module SHA256: f35fdb2086ed22c669a301f5cb801f5d9dbfc491032508e765598634fbe0a7a9.
Actual bit37 DP-SID interrupts reached the new handler, one native HPD IRQ was
forwarded, and no recovery retry was needed. This establishes boot and connected
monitor activation with the IRQ-only kernel; physical HDMI remove/return and
repeated-cycle qualification on this final kernel remain TODO. Earlier paused
checkpoint sections below describe historical deployments, not the current one.

Recovery menu entry `M4 before unified kernel` selects
`/boot/asahi/Image.before-unified-1`. Normal boot remains graphical. The external
module still requires explicit activation; boot autoload and broader feature
TODOs remain. No sleep/resume or DCP restart qualification is claimed.
Workspace evidence: `_provenance/displayport-v1-20260917/unified-kernel-1/` and
`desktop-unified-kernel-1/`. Development helpers now select the canonical kernel
source instead of the historical DisplayPort checkout.

`m4_dcpext_desktop.ko` is the explicit-load first desktop driver for the
qualified J713 DCPEXT0 / USB-C route. It registers a DRM device with live EDID,
native firmware modes, atomic modesetting and a CPU-copy primary plane. The
existing compositor discovers the card through the normal DRM hotplug event.
The normal desktop has no test-duration timeout.

Build from the workspace root (the separate MO directory is required):

```
RUSTUP_TOOLCHAIN=1.98.0 make -C build/linux-asahi-j713 ARCH=arm64 LLVM=1 \
  M="$PWD/linux/linux-asahi/drivers/gpu/drm/apple/m4-dcpext" \
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

## Failed repeated reconnect (2026-09-17)

After the successful physical remove/return check, the user reported variable
reconnect delays and one connection with no image after80seconds. Same boot
7eea6182-6162-4207-ae7a-77da24fafa25, installed desktop-dp-sid-1. The connector
was disconnected while the worker remained alive (result0,312frames); counters
were3connects/4disconnects and DPTX errors22/23/24 were10/0/2.

Retained log: HPD-high command acknowledged at365.922s, repeated command22
link-error notifications, discovery timed out at370.887s, then HPD-low was
acknowledged and the driver waited for a cable change. Thus the80second wait
was not merely the1Hz status polling delay. Exact link-error cause and recovery
remain unresolved; do not extend the discovery timeout without evidence.

Evidence: workspace _provenance/displayport-v1-20260917/dp-sid-reconnect-failure/
contains state, kernel log and raw AFK/syslog/EDID/native captures. Prior single
hotplug pass remains valid but does not establish repeated-reconnect reliability.
TODO: diagnose link failure and recovery; implement interrupt-driven DP SID
notification. User explicitly accepts dev-branch progress with known failures.


## Reconnect display release and bounded recovery (2026-09-17)

Checkpoint `99fa985a855b` preserves the failed repeated reconnect above. The
candidate now releases the firmware display request after every acknowledged
HPD-low, including same-port HDMI removal. Return reconnects and requests the
display before HPD-high, matching the ordinary DCP disconnect/connect lifecycle.
The AUX-disable callback parks the crossbar even when discovery failed before
any main-link rate was configured. No speculative PHY reset is introduced.

Discovery timeout retains its five-second bound and permits at most two recovery
attempts per physical connection, each after acknowledged HPD-low and display
release. Lost command acknowledgments remain fatal; retries never discard
uncertain ownership. A real cable/orientation loss resets the attempt budget.
`desktop_hpd_retries` reports the total recovery attempts.

`desktop-reconnect-release-1`, module SHA256
`1beeb293992d63547aecf26deb16de4f93ff41821bfd05ca2ad8c76bcf917d54`, passed the
W=1 build, actual-code ASan/UBSan lifecycle/AUX ordering tests, and guarded normal
Wayland activation on boot `ba95e8f9-b0d8-4f07-8682-ad782b21db1b`. Both outputs
were active, Philips identity/modes present, error counters zero and peak
observed temperature 57.75 C. The module is installed and lifecycle service
active. Physical HDMI removal and return passed in the same Hyprland session.
The release ACK disabled AUX; reconnection acknowledged connect/request, enabled
AUX, and published fresh modes/EDID 0.47 seconds after beginning the new route.
DPTX error counters stayed 0/0/0 and no recovery retry was needed. These times
start at driver detection, not at the operator's physical insertion. Evidence:
`_provenance/displayport-v1-20260917/desktop-reconnect-release-1/`, plus
`experiments/20260917T122356654833Z-reconnect-release-removal-1` and
`experiments/20260917T122511499906Z-reconnect-release-return-1`. The automatic
recovery branch has host fault-injection coverage; it was not needed in this
physical cycle and is not yet hardware-qualified. Longer repeated-cycle
reliability is not established by this single successful cycle.

This is a lifecycle/recovery correction, not proof of the electrical cause of
all device-not-responding notifications. Repeated reconnect reliability and
DP-SID interrupt integration remain TODO; this build still uses 1 Hz refresh.

## Interrupt integration checkpoint — paused at user request (2026-09-17)

The final IRQ-only candidate is built and committed as work in progress, **not
installed or hardware-qualified**. It admits SN201202x DP SID event bit37,
refreshes DP SID in the owner's threaded IRQ handler, acknowledges all11 event
bytes with error checking, and removes the1Hz bus polling from the snapshot API.
The main kernel also receives the existing DP mux notification fix so unchanged
pin assignment does not suppress HPD changes. Other controllers retain8-byte
interrupt access. Source-level ASan/UBSan handler, mask, snapshot and mux tests
pass; the ARM64 Image build succeeds.

Evidence for bit37: bounded diagnostic `20260917T125445712634Z-dp-sid-mask-admission-1`
temporarily added this bit while HDMI was out. On physical reconnection IRQ81
advanced14->16 before DRM reported connected. The exact prior mask was restored
and the diagnostic module unloaded. This supports the event selection; it does
not yet prove the final handler works with polling removed. No speculative
DATA_CONTROL acknowledgment or SPMI write-completion change is in the candidate.

The earlier11-byte candidate mislabeled vendor-status bits72/75 as power/data.
Native vtable resolution identifies normalized9/10 as CFVidStatus notifications;
that candidate failed physical IRQ removal qualification. Those definitions and
handlers are absent from the committed candidate. Keep this failed experiment
as evidence, not a passing interrupt-integration claim.

**Desktop left running:** boot c467995a-66bb-49bc-937c-0131ea644566, kernel#12,
Image SHA2569a3ba2c2c4a7a8f277224680ea6ac1328a8f259a602410dbe7ea164bfb5105e9,
module desktop-reconnect-release-1 SHA256
1beeb293992d63547aecf26deb16de4f93ff41821bfd05ca2ad8c76bcf917d54.
This still uses the1Hz fallback and has the experimental vendor mask
bytes enabled. Normal dual-output activation passed; the user reconnected HDMI
and DRM reports connected. The lifecycle/display-manager services remain active,
and the60-second watchdog is idle/armed. No new kernel deployment or reboot was
performed after the user requested this pause. Previous working Image backup:
/boot/asahi/Image.before-extended-irq-1. Desktop module is explicitly activated,
not automatically loaded on boot.

**Resume:** freeze/install the final candidate from
_provenance/displayport-v1-20260917/irq-integration-checkpoint/ after checking
current source/binary identity, reboot (never reload DCP in the same boot), then
use guarded desktop activation. Qualify HDMI removal/return with polling absent,
verify actual bit37 logs, unchanged compositor session, fresh EDID and native
IRQ-command ACKs. Then assess repeated reconnect/recovery. Final Image SHA256:
78fa565838385d4d3cfb762c88d89b14dd6375fb62d72234aebf1aa4c7d72105.
TODO: IRQ-only hardware qualification, generic Type-C/PHY consumer integration,
repeated-cycle reliability and timeout recovery qualification; existing broader
feature TODOs remain. Deep sleep remains explicitly deferred.

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
