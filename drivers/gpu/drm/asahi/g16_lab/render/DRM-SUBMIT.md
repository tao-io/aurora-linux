# J713 client buffer submission

`render_drm_submit=1` connects Asahi `QUEUE_CREATE`, `QUEUE_DESTROY` and
`SUBMIT` to the retained M4 firmware worker. It remains the root-only
`g16-lab` device, with opt-in private Mesa admission.

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
multi-command submissions and sustained scheduling
remain integration work.

The subsequent Mesa initialization step adds binary and timeline sync objects.
Input points must already have a submitted fence; their wait is bounded before
publication. Output points contain the actual GPU completion fence. Standalone
exported sync files pin the module until their last fence reference disappears.
VM-private GEM objects reject export and binding to a replacement VM. The lab
still retains explicit bindings until unbind/VM destruction; matching Mesa's
full GEM-close/unmap lifecycle remains work for sustained applications.

The platform device exposes the retained SGX device-tree identity to libdrm.
GET_PARAMS becomes available after the powered bootstrap probe and derives
revision/core topology from those observed registers. On the tested J713 these
are G16G B1, ten core positions and mask `0x3bd` (eight active cores). The Asahi
user timestamp unit is nanoseconds; raw firmware counters remain private and
timestamp commands are still rejected. `stage=drm-frontend` performs no GPU
probe and returns EAGAIN for the identity query.

## Native Vulkan pass

The native Mesa path additionally consumes the empty attachment-set header and
the four appended resource-count high dwords. It strips USC pointer tags and
programs M4 descriptor fields for background/EOT resources, pass multisampling,
scissor/depth-bias/query addresses, clear values and empty-tile processing.
Admission remains bounded to one 256x256 RGBA8 pass with one sample and no
depth/stencil, shader scratch, timestamp or sampler-heap use.

`tools/hyprland/submit.c` creates real Vulkan shaders, pipeline, image, vertex
buffer and render pass. It records once, changes vertex colors between eight
serial submissions, and checks every output pixel. Readback uses the known
M4 uncompressed RGBA8 Morton layout directly through host-visible memory;
this is a bring-up check, not a general Vulkan image-copy implementation.
The diagonal coverage boundary permits either valid edge convention; triangle
channels allow one UNORM unit, and clear pixels must be exactly zero.

J713 run `20260908T002333Z-render-ta` passed all eight Vulkan submissions and
524,288 pixel checks, with eight TA/fragment completions, no dropped KTrace
records, clean module unload and watchdog disarm after evidence collection.
See `validation/j713-vulkan-submit-20260908.json`. Mesa supplied all shaders,
commands and render-target storage; the kernel's fixture resources were not
used for these draws. Texture sampling, blending, WSI, Zink and Hyprland remain
separate work.

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

`validation/j713-mesa-device-20260908.json` records the later run
`20260907T235422Z-render-ta`: a private ARM Honeykrisp build created both a
Vulkan physical device and a logical device/graphics queue on the M4. It then
closed them without submitting graphics work. The same run subsequently passed
the eight client-fixture draws, binary/timeline completion checks, standalone
sync-file lifetime check and complete module teardown. This is device
initialization evidence, not a Vulkan-rendered image.

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
