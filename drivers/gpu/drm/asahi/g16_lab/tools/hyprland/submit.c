/* SPDX-License-Identifier: MIT */
/* Real GPU submission. Never use as run-render.py --mesa-device-probe. */
#define M4_PIPELINE_PROBE
#define M4_SUBMIT_PROBE
#include "device.c"
