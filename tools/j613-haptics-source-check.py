#!/usr/bin/env python3
"""Reject the incomplete plain source copy previously used for D3 packaging."""
from pathlib import Path
import sys
k=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[1]
d=(k/'drivers/hid/dockchannel-hid/dockchannel-hid.c').read_text();s=(k/'drivers/hid/spi-hid/spi-hid-apple-core.c').read_text()
assert 'dchid_cmd(iface, rtype, REQ_SET_REPORT' in d,'Missing 0001'
assert 'dchid_cmd(iface, HID_OUTPUT_REPORT, REQ_SET_REPORT' not in d
assert '#define SPIHID_MAX_DEVICES 4' in s and 'struct spihid_interface aux;' in s,'Missing 0002'
assert 'reportnum != idev->id' not in s and '0x51, buf[0]' in s,'Missing 0003'
m=(k/'drivers/hid/hid-magicmouse.c').read_text();a=(k/'drivers/hid/hid-apple-mtp-haptic.c').read_text()
for token in ('magicmouse_haptic_forget_mode','haptic_deep_release_threshold','haptic_deep_waveform','Force Click'):assert token in m,token
assert 'actuator_set_mode' in a and 'HID_HAPTIC_MODE_DEVICE' in a
print('D3 haptics transport and force-stage source checks PASS')
