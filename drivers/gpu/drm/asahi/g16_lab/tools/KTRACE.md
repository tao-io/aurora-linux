# J713 firmware execution trace

The linux-alpha Rust driver already allocates `KTraceChannel`; m1n1-alpha
has `GPUKTraceChannel` and a `KTraceMsg` decoder. Both provided the lead for
collecting firmware execution diagnostics in the C lab. Their older 0x38-byte
record definition must not be copied into this M4 25G83 collector.

Same-build firmware evidence (J713 `j713-gfx-firmware.bin.c`):

- `0x245bc`: trace writer, runtime +0x1e0 state, +0x1e8 ring; 512 slots,
  0x48-byte stride, consumer +0, producer +0x20. Type 5, unaligned timestamp
  +4, four 64-bit arguments +12, event ID +44 (low 24 bits), thread +47,
  phase +48 (32 bits). Preserve the unexplained final 20 bytes.
- `0x248f0`: overflow event 0x11c, arguments count and elapsed ticks. Firmware
  can drop records when the host cannot keep up; check this explicitly.
- `0x3430`: imports trace mask from the first word of InitData root[0x15].
  Bit 0 enables basic events; bit 1 enables detailed events. No firmware
  instruction patch is needed.
- Microsequence cases 5/6, 7/8 and 0xb/0xc emit TA, 3D and compute start/finish
  events. These mark firmware processing; alone they are not shader-output
  verification. Stamp-update events additionally identify the completed job.

The collector is opt-in (`firmware_ktrace_mask=3`), drains bounded batches
alongside existing statistics and during boot, then exports a chronological
raw stream after firmware stops. It retains at most 8192 records and reports
host drops separately from firmware overflow records. It uses the existing
lab cache synchronization and UAT lifetime. Dynamic debug can optionally
stream individual events; normal operation avoids printing each record.

Install `g16ktrace.py` beside the runner and watchdog module, then run:

```
python3 run-mandelbrot.py --module ./g16_lab.ko --firmware-ktrace 3
python3 run-mandelbrot.py --module ./g16_lab.ko --render-loader-marker --firmware-ktrace 3
```

These require separate fresh boots. The runner arms and verifies the
60-second watchdog before either experiment and saves raw and decoded traces,
checksums, loss counters, kernel logs and the manifest before disarming.
An empty, malformed or lossy requested trace fails its diagnostics check.
The decoder can also run offline:

```
python3 g16ktrace.py firmware-ktrace.bin --timeline
python3 g16ktrace.py firmware-ktrace.bin > firmware-ktrace.json
python3 test-g16ktrace.py
```

Timestamps remain firmware ticks; no guessed timebase or historical thread
names are assigned. Unknown events and all raw bytes remain available.

## Hardware evidence, 2026-09-07

- 195911Z, boot 1ee8e6b0-b59a-4952-b7cd-95833e3963f2: 976 records, zero
  host drops and firmware-reported losses, start/finish/compute stamp events.
  Mandelbrot retained its exact expected SHA256 and passed.
- 200100Z, boot daa94378-d492-49e7-bf8f-ef3c32493687: 1458 records, zero
  host drops and firmware-reported losses. The TA events identify UUID
  0x16000001, queue 0xfffffc3000010000, completion stamp 0x100 at
  0xfffffc300000c900 and event 112. The marker words and tilemap remain
  unchanged, so the render test correctly fails. No fragment job was submitted.

Compressed raw traces, manifests and summaries are under `render/validation/`.
These are native Linux firmware traces from M4, not macOS framework traces
or a transferred Neo hardware-success claim.

The integrated runner was then verified in 200449Z (boot
87743ead-51fc-4482-b63d-5020d6acb282) with mask 1: 20 records, no losses,
and `firmware_ktrace_valid: true`. Basic mode retains the relevant start,
finish and completion events with substantially less volume. Its manifest
correctly keeps the overall render result failed because output verification
has not passed. Decoder tests cover both malformed synthetic inputs and the
actual compute/TA captures.
