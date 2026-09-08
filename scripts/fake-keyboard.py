#!/usr/bin/env python3
"""Plug a pretend USB keyboard in for N seconds, to exercise the live
touch-controls hot-plug path without hardware.

    scripts/fake-keyboard.py 10        # appears for 10 s, then goes away

Needs write access to /dev/uinput (the `input` group or an ACL) and
python-evdev. The name deliberately avoids "virtual": the detector ignores
the permanently-present virtual keyboards remappers create.
"""
import sys
import time

from evdev import UInput, ecodes as e

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
keys = [getattr(e, "KEY_%s" % c) for c in "QWERTYUIOPASDFGHJKLZXCVBNM"]
keys += [e.KEY_ENTER, e.KEY_SPACE, e.KEY_LEFTSHIFT, e.KEY_ESC, e.KEY_1, e.KEY_0]
with UInput({e.EV_KEY: keys}, name="Test USB Keyboard", vendor=0x046d,
            product=0xc31c, version=1, bustype=e.BUS_USB) as ui:
    print("fake keyboard attached as", ui.device.path, flush=True)
    time.sleep(seconds)
print("fake keyboard removed", flush=True)
