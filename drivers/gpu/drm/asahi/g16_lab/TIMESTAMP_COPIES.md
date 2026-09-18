# Render timestamp copies (experimental)

Feature bit 4 extends the render payload with eight additional fragment-end
timestamp destinations. Old 256-byte payloads are zero-extended. New destinations
are bound timestamp-object handles and eight-byte aligned offsets, resolved and
retained at submission. A source fragment-end timestamp is mandatory. G17
translation refuses the extension; only the M4 runtime advertises it.

After exact engine, queue and pipe retirement, copy the original firmware-written
64-bit value into each additional destination before signalling the packet fence
or admitting dependent work. Aligned volatile accesses preserve atomic query
availability reads. No host-clock sampling, clock conversion, extra firmware
command, relaxed retirement condition or queue-depth change is involved.

Motivation: the untouched Factorio menu issues multiple timestamps within one
render pass. Mesa already uses that pass's end value for all of them, but emits
five extra compute commands per typical frame to copy seven values.

Host check: tools/test-g16-timestamp-copy-abi.py compiles the production parser
section and checks old payloads, all eight destinations, and unknown nonzero
tail rejection. The complete g17_uapi standalone test target currently fails in
unrelated G17 module imports; this narrow check avoids those modules.
Kernel module and ARM64/x86 Mesa builds passed. Hardware qualification pending.
Mesa use is opt-in via HK_M4_TIMESTAMP_COPIES=true and requires feature bit 4.

TODO: measure Factorio FPS, validate reused queries and GPU/CPU readback, and
qualify broader Vulkan query coverage before considering default enablement.
