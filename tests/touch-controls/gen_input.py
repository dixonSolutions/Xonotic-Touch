#!/usr/bin/env python3
"""gen_input.py W DPI CM > tour: the scripted-finger input tour (finj.c format)."""
import sys
W, DPI, CM = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
dx = CM / 2.54 * DPI / W      # CM centimetres as a fraction of the width
out = []
def cmd(t, s): out.append(f"{t:.3f} cmd {s}")
def mark(t, name): cmd(t, f"echo MARK {name}"); cmd(t + 0.01, "touch_hud_state")
def drag(t, fid, x, y, dxn, dur, down=True, up=True):
    if down: out.append(f"{t:.3f} fdown {fid} {x:.5f} {y:.5f}")
    n = max(2, int(dur * 60))
    for i in range(1, n + 1):
        out.append(f"{t + 0.05 + dur * i / n:.3f} fmove {fid} {x + dxn * i / n:.5f} {y:.5f}")
    if up: out.append(f"{t + dur + 0.15:.3f} fup {fid} {x + dxn:.5f} {y:.5f}")
# In the game, whatever was up when the tour starts (the join dialog on a
# server); the acceleration is off until the flick, so every drag before it
# is exactly its length whatever the frame pacing of a loaded machine.
for t in (0, 1.5, 3.0):
    cmd(t, "touch_join auto"); cmd(t + 0.3, "togglemenu 0")
cmd(0.1, "touch_look_accel 0")
mark(4.2, "base")
drag(4.5, 1, 0.45, 0.40, dx, 1.0)            # slow look swipe, CM to the right
mark(6.2, "slow")
out.append("7.000 fdown 2 0.85500 0.68000")   # FIRE
mark(7.3, "fire_held")
drag(7.5, 2, 0.855, 0.68, -dx, 1.0, down=False, up=False)
mark(8.8, "fire_dragged")
out.append(f"9.000 fup 2 {0.855 - dx:.5f} 0.68000")
mark(9.4, "fire_up")
out.append("10.000 fdown 3 0.85500 0.88500")  # HOP, held
drag(10.3, 3, 0.855, 0.885, -dx, 1.0, down=False, up=False)
mark(11.6, "hop_dragged")
t = 11.7
while t < 15.0:
    cmd(t, "touch_hud_state"); t += 0.1
out.append(f"15.000 fup 3 {0.855 - dx:.5f} 0.88500")
mark(15.5, "hop_up")
out.append("16.000 fdown 4 0.12000 0.80000")  # stick: lands, pushes up and right
for i in range(1, 13):
    out.append(f"{16.05 + i * 0.016:.3f} fmove 4 {0.12 + 0.05 * i / 12:.5f} {0.80 - 0.08 * i / 12:.5f}")
mark(16.6, "stick_held")
out.append("17.000 fup 4 0.17000 0.72000")
mark(17.4, "stick_up")
cmd(17.6, "touch_look_accel 0.8")
drag(18, 1, 0.45, 0.40, dx, 0.08)             # the same travel as a flick
mark(18.6, "flick")
cmd(19.5, "quit")
print("\n".join(out))
