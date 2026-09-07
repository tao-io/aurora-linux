# J713 client buffer submission

`render_drm_submit=1` connects Asahi `QUEUE_CREATE`, `QUEUE_DESTROY` and
`SUBMIT` to the retained M4 firmware worker. It remains the root-only
`g16-lab` device, without Vulkan device admission.

Each submission snapshots the command and sync arrays, validates its queue and
VM, and builds a new UAT containing the client's GEM mappings plus the owned
kernel graph and parameter pool. Root 7 remains reserved for the enclosing
firmware owner. The worker switches context 6 under the UAT handoff lock while
both engines are idle, executes the supplied VDM/BG/EOT addresses, and restores
the original root before completing the job. Input/output cache maintenance,
GEM reservation fences and binary syncobjs accompany that operation.

The submitting ioctl holds the VM lock through completion. Closing GEM handles
cannot release bound pages. On failure, the ioctl and its fences remain pending
until the enclosing bootstrap has stopped firmware; backing and UAT tables are
not freed merely because a wait or dispatch failed. Calls are synchronous for
this initial backend. All client BO reservations are conservatively treated as
writes, and imports/cross-file buffer sharing are not qualified.

The admitted command subset is deliberately small: one render command,
256x256 RGBA8, one layer/sample, 32x32 utiles, USC base `0x10000000000`, and
medium-priority queues. VDM addresses use the M4 compact aperture. Nonzero
unsupported fields, missing program mappings, invalid sync objects and stale
queue IDs fail instead of being silently ignored. The worker still has its
eight-draw/20-second limit. General texture/blend/attachment commands,
multi-command submissions, timeline synchronization and sustained scheduling
remain integration work.

## Hardware evidence

`validation/j713-drm-submit-20260908.json` records the successful J713/T8132/G16G,
25G83 run `20260907T233825Z-render-ta`. The client copied the prepared M4 shader
and command fixtures into **34 new GEM objects**, bound them into its VM, closed
all GEM handles, and submitted eight Asahi commands. Every draw changed vertex
colors in userspace. Independent CPU checks matched all 524,288 output pixels;
eight exported binary sync files had successful status. Firmware KTrace recorded
eight TA and eight fragment completions without lost records. Unload removed
the render node, no kernel fault was reported, and the 60-second watchdog was
disarmed only after evidence was saved.

This qualifies client buffer execution and the initial submission backend.
The fixtures still supply the initial command programs; it does not qualify
Honeykrisp-generated render passes or accelerated Hyprland.

Two preceding attempts stopped in the test reader before any render submission:
debugfs blob sizes are zero in `stat`, and the first reader update did not
survive the watchdog reset. The final reader reads through EOF; deployment was
synced and hashes verified after boot. Both failed attempts retain diagnostics.

Build `tools/g16submittest.c` with `-std=gnu11 -O2 -Wall -Wextra -Werror`, the
pinned Aurora Mesa `asahi_drm.h` and the system libdrm headers. On a fresh J713
boot, use the guarded runner:

```
PYTHONPATH=/root python3 /root/g16-submit-test/run-render.py \
  --module /root/g16-submit-test/g16_lab.ko --render-drm-submit \
  --drm-submit-test /root/g16-submit-test/g16submittest \
  --firmware-ktrace 3 --results /root/g16-render-results
```

The runner arms and verifies `/dev/watchdog0` before loading the module. Do not
run the C test or load the module directly as an unguarded experiment.
