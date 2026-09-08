#!/usr/bin/env python3
"""Inject real touch events through a uinput touchscreen.

Usage: fake-touch.py [--w W --h H] [--rot 0|1|2|3] ACTION...
  ACTION:  tap:X:Y[:HOLD_MS]   drag:X1:Y1:X2:Y2:MS   wait:MS
X/Y are logical screen pixels (default 2880x1920 landscape).  --rot applies the
panel transform mutter reports for the output (1 = 90 deg) so the device frame
matches the panel's native orientation.
"""
import sys, time, argparse
from evdev import UInput, AbsInfo, ecodes as e

p = argparse.ArgumentParser()
p.add_argument('--w', type=int, default=2880)
p.add_argument('--h', type=int, default=1920)
p.add_argument('--rot', type=int, default=0)
p.add_argument('--settle', type=float, default=1.2)
p.add_argument('actions', nargs='+')
a = p.parse_args()

# Device frame: for rot 1/3 the panel is natively portrait (h x w).
dw, dh = (a.w, a.h) if a.rot in (0, 2) else (a.h, a.w)
cap = {
    e.EV_KEY: [e.BTN_TOUCH],
    e.EV_ABS: [
        (e.ABS_X, AbsInfo(0, 0, dw - 1, 0, 0, 0)),
        (e.ABS_Y, AbsInfo(0, 0, dh - 1, 0, 0, 0)),
        (e.ABS_MT_SLOT, AbsInfo(0, 0, 9, 0, 0, 0)),
        (e.ABS_MT_TRACKING_ID, AbsInfo(0, 0, 65535, 0, 0, 0)),
        (e.ABS_MT_POSITION_X, AbsInfo(0, 0, dw - 1, 0, 0, 0)),
        (e.ABS_MT_POSITION_Y, AbsInfo(0, 0, dh - 1, 0, 0, 0)),
    ],
}
ui = UInput(cap, name='Fake Touchscreen', vendor=0x1234, product=0x5678,
            version=1, input_props=[e.INPUT_PROP_DIRECT])
time.sleep(a.settle)
tid = 100

def dev_xy(x, y):
    x = max(0, min(a.w - 1, int(x))); y = max(0, min(a.h - 1, int(y)))
    if a.rot == 0: return x, y
    if a.rot == 1: return y, a.w - 1 - x           # 90 deg
    if a.rot == 2: return a.w - 1 - x, a.h - 1 - y
    return a.h - 1 - y, x                           # 270 deg

def down(x, y):
    global tid
    tid += 1
    dx, dy = dev_xy(x, y)
    ui.write(e.EV_ABS, e.ABS_MT_SLOT, 0)
    ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, tid)
    ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, dx)
    ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, dy)
    ui.write(e.EV_KEY, e.BTN_TOUCH, 1)
    ui.write(e.EV_ABS, e.ABS_X, dx)
    ui.write(e.EV_ABS, e.ABS_Y, dy)
    ui.syn()

def move(x, y):
    dx, dy = dev_xy(x, y)
    ui.write(e.EV_ABS, e.ABS_MT_SLOT, 0)
    ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, dx)
    ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, dy)
    ui.write(e.EV_ABS, e.ABS_X, dx)
    ui.write(e.EV_ABS, e.ABS_Y, dy)
    ui.syn()

def up():
    ui.write(e.EV_ABS, e.ABS_MT_SLOT, 0)
    ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, -1)
    ui.write(e.EV_KEY, e.BTN_TOUCH, 0)
    ui.syn()

for act in a.actions:
    f = act.split(':')
    if f[0] == 'tap':
        x, y = float(f[1]), float(f[2]); hold = float(f[3]) / 1000 if len(f) > 3 else 0.09
        down(x, y); time.sleep(hold); up(); print('tap', x, y, flush=True)
    elif f[0] == 'drag':
        x1, y1, x2, y2, ms = map(float, f[1:6]); steps = max(2, int(ms / 16))
        down(x1, y1)
        for i in range(1, steps + 1):
            t = i / steps; move(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t); time.sleep(ms / 1000 / steps)
        up(); print('drag', x1, y1, x2, y2, flush=True)
    elif f[0] == 'wait':
        time.sleep(float(f[1]) / 1000)
    else:
        sys.exit('bad action ' + act)
time.sleep(0.3)
ui.close()
