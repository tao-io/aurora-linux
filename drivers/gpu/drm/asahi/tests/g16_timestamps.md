# M4 firmware timestamp destinations

The common Asahi Timestamp operation has separate internal tick pointers and
user result pointers. The 25G83 M4 opcode is 3 (start flag bit31), 0x4c bytes.
Its user pointer pair is at operation+36; internal pointers are at +12.
The firmware converts user results to nanoseconds with ticks * 125 / 3.

Offline evidence: saved J713 firmware SHA256
`aee1042e536386a90437826f36b2c469c06d29370588e107372dafe8fda93ef7`.
Opcode dispatch table TEXT1c4c0, dispatcher-relative base TEXT18f4c, entry2
selects TEXT19364. TEXT19494..19520 accepts result pointers only in
`Runtime->Main+0x28 .. base+0x3fffff8`, a 64 MiB timestamp aperture.
The previous M4 configuration put its SGX MMIO mapping at Main+0x28 and
mapped query buffers in the general allocation range. Firmware therefore
ignored the user destinations. Main+0x28 is timestamp_area_base, as in
common Asahi HwDataB, and must agree with map_timestamp_buffer.

The kernel CPU-publication path also failed a GPU-reset ordering probe:
20260912T104420670830Z wrote a valid first timestamp which was overwritten
by the query reset sentinel. Reset-only probe104734 passed immediately
after its fence; query WC allocation104846 did not repair publication.
Direct firmware destinations with the old aperture105404 were ignored.
The first corrected aperture105921 was outside the firmware VM allocator
and failed GEM_BIND_OBJECT before GPU execution. The revised partition
reserves the last64 MiB of the existing firmware VM for timestamp buffers.

The revised partition and final timestamp delivery passed the standalone
probe as recorded below; broader replay and desktop results remain separate.
The standalone Vulkan probe lives in Mesa tests/m4_vulkan_timestamp_query.c.
Every hardware execution uses the workspace watchdog and thermal guard.

Corrected partition110258: firmware writes both user results, matching internal
hardware ticks converted to ns exactly. The first is still overwritten by the
GPU reset. Thus aperture correction is established separately from ordering.
Strong CDM release110555 encoded0x60001560 after each dispatch, verified in the
saved control stream, but did not prevent that overwrite. Next diagnostic
retains native internal timing and adds user publication after Finalize.

Final qualification112314: both timestamps nonzero and ordered, available on
first read after fence, peak54.5C, noGPUfault. Firmware uses native before-
Finalize Timestamp operations with their user pointer pair. AllCPU result
publication/comparison code and the unsuccessful post-Finalize extraoperation
were removed. Mesa preserves a GPU job boundary after timestamp resets so
GPU writes retire before firmware takes ownership of the query storage.
Ordinary CDM merging stays enabled. Mesa commitdb28bd35934; moduleSHA256
f7a376b3be417c4ba4220dbed20b0ff90f47fb925439d6a58c1ae6a4a5b3e549.

This qualifies the standalone compute timestamp path. Render timestamps
remain rejected; no general Vulkan query or game-support claim follows.
`firmware-disasm.py` preserves a bounded offline disassembly and its image
hash/range/command; the workspace copy is under tools/.
