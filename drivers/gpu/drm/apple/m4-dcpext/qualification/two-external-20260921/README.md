# Two external displays: implementation checkpoint, 2026-09-21

Initial live diagnosis: both USB-C controllers have partners, but Linux exposes
only one external DRM connector, alongside eDP. The running implementation uses
DCPEXT0 and migrates it between PHYs. Firmware/DART reservations for DCPEXT1 are
already present in the bootloader handoff (UUID matches DCPEXT0).

Instantiate the existing runtime separately for DCPEXT1, with distinct sysfs,
debugfs, root device, transport/EDID/DMA/DRM state and lifecycle. Share only the
stateless helpers and mutex-protected physical PHY arbiter. A route must be
claimed before mapping or programming; retirement releases it, failures retain
it. Each controller admits its own dispextN firmware service publications.
The existing recovery parameter disabling the primary disables both instances.

Saved J713 ADT: DCPEXT1 UFP endpoint 2 /port 0, native selector 4. Existing
DCPEXT0 endpoint 1 uses selector 2. Generalize clock/selector bitfields and verify
the complete up/down sequence against offline execution of saved native code.
Only register facts are used in the driver; no native machine code is embedded.

Offline tests: actual selector2 retains its native write sequence and fault
checks; selector4 matches native writes/delays; malformed selectors never write.
The actual PHY arbiter passes migration/foreign-release checks and 200,000
concurrent claim attempts. AFK publication tests verify per-controller names;
existing boot and runtime hotplug tests pass. Image/modules build succeeds.
Hardware validation follows; no two-external-monitor success claimed yet.

TODO: general platform-device contexts instead of compiling the runtime twice;
DT graph/power management integration; suspend/resume remains deferred.
