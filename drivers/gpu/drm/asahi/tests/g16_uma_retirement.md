# M4/25G83 private-memory owner retirement

Normal SDDM login on J713 at 2026-09-14 17:25:40 UTC faulted in Hyprland
job301, ASID6. gUPM (requestor 0xd9) read unmapped VA 0x7100490e80:
Quickshell's immediately preceding ASID8 freelist 0x7100490000 + 0xe80.
The current UMA descriptor and hardware slot1 both pointed to Hyprland's
correct freelist 0x7104960000. Descriptor replacement alone had not retired
the old hardware allocator state. Linux and SSH stayed alive; the GPU became
unavailable and both desktop clients received DEVICE_LOST.

The saved fault metadata shows that the host count at metadata+0x500 was
zero. The driver also left every compute owner's corresponding count zero.
This count is distinct from RunCommand+4 and the start-record command ID.

Native AGXUSCPrivMemFList::updateSubmitInfo (25G83 TEXT 8a0683c..48)
increments the host count once for each submitted engine. Initial firmware
state (8a072fc, 8a07308..310) binds its firmware alias at UMA+4c and initializes
the firmware completion count at UMA+54 from the previous submitted count.
Firmware increments UMA+54 on final engine completion (1a7e8..1a7f8;
other finish paths at 19a98..19aa8 and 1ad5c..1ad6c).
Partial fragment work does not add another host submission.

Firmware TEXT 2b550..560 returns without draining when *UMA+4c != UMA+54.
When equal, the 2b5cc..670 path drains slot+ASID state through SGX 0x14008,
waits at 324f8, copies the hardware cursors back, and clears its cached owner
pointer. Installing another descriptor (2af18) is not a replacement for
this completion protocol.

Each Linux owner is freshly initialized before publication: completed=0,
submitted=2 for the shared TA/fragment pool, or submitted=1 for compute.
The host count is published with the commands, so completion of the tiler
alone cannot drain a pool still needed by the final fragment. Recycled render
owners reset both counts along with their existing allocator metadata.
The existing retained-owner lifetime rules remain in force.

Original sealed fault dump: 1,666,968 bytes, SHA256
239ae0f1bbd70dbd95f6fd9d02096afe083711ade2b8070c76abe3b324a7c7e6.
Workspace evidence: build/desktop-crash-20260914-1726/.
Native listings: build/0ad/native-agx-full.S and firmware-full.S.

ARM64 module build and six compute wire tests pass. Guarded J713 check
20260914T174535306825Z-uma-submitted-depth-clients passes 8 paired rounds
(16 independent Zink processes), with 48 render jobs, 47 observed render ASID
transitions, and exact color/depth readbacks. Firmware reports render2/2 and
compute1/1 completion counts. Six-channel monitoring peaked at59.5C with the
60-second watchdog. The initial MRT4/mixed attempt had no GPU fault but failed
its pixel oracle: its floating-point clear of integer RT1 was undefined.
The probe was corrected to use an integer clear and passed host llvmpipe.
Normal login validation remains pending recovery from another watchdog lease.
