# M4 attachmentless parameter heap regression

Satisfactory DX12/VKD3D submits an attachmentless 16384 x 16384 framebuffer.
Mesa's inherited maximum-framebuffer workaround submits 32 layers. Multiplying
its 40 MiB tile table by 128 requested a 5 GiB physical parameter heap. Two
startup runs failed allocation; the second invoked the host OOM killer.

Use common Asahi single-cluster minimum sizing: one 128 KiB block per 128
screen tiles, rounded to eight blocks, retaining the existing 4 MiB floor.
The heap is recyclable through the existing partial-render path. Per-layer
tile and TPC tables remain fully allocated. This reduces that heap to 256 MiB;
it does not reduce render dimensions, layers, samples, or shader output.

`probe.c` submits that exact geometry, rasterizes a 64 x 64 viewport without
attachments, and checks all 4096 fragment SSBO writes against independent
CPU expectations. Compile with `cc -O2 -Wall -Wextra probe.c -lvulkan -o probe`.
The checked-in shader headers are SPIR-V compiled from the adjacent GLSL with
glslangValidator -V. Hardware runs require the mandatory experiment guard.
`PROBE_SOFTWARE=1` permits a CPU Vulkan reference only; M4 is checked by ID.

Validation on 2026-09-25:
- 17 host render/compute ABI tests pass, including maximum layered geometry.
- Module builds with the qualified Rust 1.98.0 toolchain.
- Host lavapipe passes all 4096 words.
- M4 same probe passes, no firmware fault, peak 73.5 C, under verified watchdog.
  Evidence: 20260925T043839381379Z-satisfactory-attachmentless-pool-oracle.
  Module SHA256 cca3c80ff012b744aa8d33e5073ce18206a6dd2669b7643027307d347a8de15b.

This proves the allocation regression and small observable draw. It does not
prove Satisfactory DX12 or every partial-render workload; full capture replay
and a new live game run remain separate validation.
