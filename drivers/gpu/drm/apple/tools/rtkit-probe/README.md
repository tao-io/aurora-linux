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
The newly added bridge still requires qualification. Native page flips and
removal of simpledrm framebuffer copies are not established by these tests.
