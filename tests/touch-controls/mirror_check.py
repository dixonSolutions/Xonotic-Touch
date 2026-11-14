#!/usr/bin/env python3
"""The port's CSQC mirrors the engine's look and stick constants
(qcsrc/client/touch_feel.qh from touch_feel.h, the stick radius floor from
touch_hud.c), and both HUDs register the same defaults for the settings the
review changed (touch_init.qc's table against touch_hud.c's cv() fallbacks)."""
import re
import sys

feel_h, hud_c, feel_qh, init_qc = (open(p, encoding="utf-8").read() for p in sys.argv[1:5])
bad = []
c = dict(re.findall(r"^#define\s+(TOUCHFEEL_\w+|TOUCHDP_STICK_RADIUS)\s+([\d.]+)f?", feel_h + hud_c, re.M))
qc = dict(re.findall(r"^const float (TOUCHFEEL_\w+|TOUCHDP_STICK_RADIUS)\s*=\s*([\d.]+);", feel_qh, re.M))
for name, value in qc.items():
    if name not in c or float(c[name]) != float(value):
        bad.append(f"{name}: QuakeC {value}, C {c.get(name)}")
if not qc:
    bad.append("no constants found in touch_feel.qh")
table = dict(re.findall(r'Touch_RegisterCvar\("(\w+)",\s*"([^"]*)"\)', init_qc))
for name in ("touch_sens_base", "touch_sens_y_mult", "touch_look_accel", "touch_look_accel_lo", "touch_look_accel_hi",
             "touch_look_fcmin", "touch_look_beta", "touch_look_filter", "touch_stick_deadzone", "touch_stick_run",
             "touch_stick_float", "touch_stick_follow", "touch_stick_analog", "touch_move_size", "touch_fire_size",
             "touch_crouch_size", "touch_fire_drag_look", "touch_button_drag_look"):
    engine = set(re.findall(r'cv\(\s*(?:aim_button \? "touch_fire_drag_look" : )?"%s",\s*([\d.]+)f?\)' % name, hud_c))
    if name == "touch_fire_drag_look":
        engine = {"1"}  # read through the ?: in button_drag_look with the default 1
    if name not in table:
        bad.append(f"{name}: not in touch_init.qc's table")
    elif not engine:
        bad.append(f"{name}: no cv() default in touch_hud.c")
    elif any(float(e) != float(table[name]) for e in engine):
        bad.append(f"{name}: QuakeC default {table[name]}, engine {sorted(engine)}")
if bad:
    for line in bad:
        print("MISMATCH", line)
    sys.exit(1)
print(f"touch_feel.qh mirrors touch_feel.h ({len(qc)} constants); 18 defaults agree between the HUD copies")
