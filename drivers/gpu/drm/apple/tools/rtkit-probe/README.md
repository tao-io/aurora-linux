# J713 native DCP diagnostics

This module is a firmware diagnostic, not a DRM/KMS display driver. It is gated
on the qualified J713 firmware UUID and retains its DMA buffers until reboot.
Never unload it while firmware may still access those buffers.

`run-native.py --bridge` starts native RTKit and DCPLink under the 60-second
watchdog and temperature guards. The optional root-only `/dev/m4-dcp-rpc`
interface sends bounded RPC packets and delivers firmware callbacks. It supports
one normal command and one nested command during a callback, not arbitrary
command concurrency. Use only one client. Unknown callbacks must be inspected;
the supplied client does not automatically acknowledge them.

`run-rpc.py --guards /root/g16-submit-test --output UNIQUE_DIRECTORY --call A412
--output-size 4 --repeat 100 --expect-hex 01000000` checks the read-only main-display
query and sequential buffer reuse. Each test independently arms the watchdog and
records temperatures and RPC evidence. A failed hardware test leaves the watchdog
armed. Reboot before loading a different module build.

The diagnostic record header is little-endian `u32 kind, u32 size, u64 message`,
followed by exactly `size` bytes. Write kind 1 submits a complete RPC packet;
message selects normal (0) or nested (1). Write kind 2 replies to a callback using
its original message and exact output size. Read kinds 1 and 2 contain complete
command-reply and callback packets; kind 3 reports an unexpected message.

Native RTKit, DCPLink READY (64-byte alignment), and A412 have hardware evidence.
The bridge passed 100 sequential A412 queries, callback placement and recursive
RPC during startup. Native page flips and
removal of simpledrm framebuffer copies are not established by these tests.

The optional hardware-start diagnostic sequences M4 methods and stops on an
unimplemented callback. `--decline-bandwidth` explicitly rejects the bandwidth
service while investigating inherited-state startup; it is not successful
bandwidth configuration. The kernel log remains part of validation even when
a method returns success. In particular, the firmware's 48 MHz host-less clock
fallback is below this panel's video clock and is not a qualified display rate.

D451 uses ioctl 0xc0204d40 with native-endian `u64 size, physical, dva; u32 id,
flags` (32 bytes). Only size is supplied. Allocation requires a matching active
D451 callback, is limited to 16 MiB each/64 MiB total/64 handles, and uses the DMA
API with contiguous physical backing verified through its scatterlist. Handles
start at one. Ioctl 0x40044d41 retires a u32 handle; memory and mappings remain
pinned until reboot. No arbitrary physical-memory mapping is exposed.

Ioctl 0xc0204d42 maps an existing allocation for PIODMA, with only `id` supplied
in the same 32-byte structure. It requires an active D201 callback and a
bootloader-qualified SID16 device; every translated page is checked before the
address is returned. All PIODMA mappings also remain pinned until reboot.

`--initialize` verifies A401's boolean success and opens the first client before
running the requested diagnostic. `--scanout-pattern` allocates one 2560x1664
BGRA8 buffer via native DISP0 SID0, fills color bars once, and submits it as
layer 0. It requires the separate m1n1-validated scanout device. Ioctl
0xc0204d43 uses `u32 width,height,stride,format; u64 size,dva`; only the exact
panel dimensions are supplied. One allocation is allowed per boot, outside
active RPC/callbacks. Memory and device references stay pinned until reboot.
This is a bring-up buffer, not a userspace framebuffer or DRM/KMS ABI.

`run-native.py --kernel-client` selects an exclusive in-kernel RPC client and
performs the qualified startup sequence, first-client-open, and background swap.
It does not expose the userspace RPC device in this mode. The native callback
handler preserves bounded numeric/raw properties, validates all known shapes,
and uses the same DMA allocator and PIODMA mapper as the userspace diagnostic.
The startup path still explicitly declines bandwidth/default-surface services
and uses the previously qualified clock fallback. Unknown callbacks fail closed.
`../m4_dcp_native_callbacks_test.py TRACE/rpc.jsonl` replays captured input
packets through the production callback handler with stubbed RPC/DMA operations
under ASan/UBSan; it is not a hardware execution test.

### Native KMS prototype

`run-native.py --kernel-client --kms` registers `m4-dcp` after the qualified
startup/background completion. It uses the firmware-published fixed J713
2560x1664 mode, a single linear XRGB8888/ARGB8888 primary plane, and GEM DMA
allocation/import on the preserved DISP0 SID0 domain. There is no CPU scanout
copy. Each atomic flush waits for matching D590 completion before releasing
the previous framebuffer or reporting a page flip. Failure latches the device
and retains both potentially live buffers until reboot. No panel power, clock,
suspend/resume, cursor plane or arbitrary modesetting is implemented yet.
The explicit-load module remains pinned; replace it by rebooting.

Use `--takeover --stop-desktop` for compositor tests: the native device removes
the conflicting simpledrm aperture before registration. The two drivers must
not both own the panel. The native module does not start a compositor itself.

On 2026-09-09, after removing the GPU lab's misplaced DISP_CPU retention writes
(commit d0aaedfbc6ad), a guarded 120-second native Hyprland session passed on
J713 boot `65b92581-db0a-49d8-9c72-b8912cf134d0`. Hyprland used the M4 Zink
renderer; the native KMS snapshot verified 3,863,593 red test-window pixels in
an imported GPU buffer. The session completed 23 independent eight-frame GPU
pixel health checks and more than 1,600 native flips with no KMS failure.
SuperTux ran through Zink for 45 seconds, about 34–40 FPS in this run; this is
not a 60 FPS game claim. All six PMP sensors were monitored, peak 72 C, with
GPU clocks unchanged. The watchdog was disarmed only after saved PASS evidence.
The normal scanout path imports DMA-BUF directly; the explicit diagnostic
snapshot still reads pixels on the CPU. Password-screen flicker remains a
separate visual validation target.

`run-greeter-test.py --output NEW_DIRECTORY` checks the installed SDDM greeter
with native KMS already active. It arms the 60-second watchdog and thermal
guard, moves only the cursor across an empty upper strip, saves cropped
scanout pixels, and verifies that each of 18 positions contains the new cursor
without stale bright pixels elsewhere. It restores the original cursor
position on success. The check is specific to the qualified 2560x1664 panel,
scale 2, and current greeter theme; it never types into the password field.
A test error saves evidence and leaves recovery armed. This does not establish
optical panel scanout order or validate arbitrary themes.
