#!/usr/bin/env python3
"""Run the actual ARM64 conversion instructions with a user-space test ABI."""
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
assembly = here.parents[3] / 'sysfb/drm_sysfb_neon.S'
source = assembly.read_text().replace(
    '#include <linux/linkage.h>',
    '#define SYM_FUNC_START(x) .global x; .type x, %function; x:\n'
    '#define SYM_FUNC_END(x) .size x, .-x')
with tempfile.TemporaryDirectory(prefix='sysfb-neon-test-') as directory:
    work = Path(directory)
    (work / 'convert.S').write_text(source)
    subprocess.run(['aarch64-linux-gnu-gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-static', str(here / 'test_sysfb_neon.c'),
                    str(work / 'convert.S'), '-o', str(work / 'test')], check=True)
    subprocess.run(['qemu-aarch64', str(work / 'test')], check=True)
