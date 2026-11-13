#!/usr/bin/env python3
"""The port's CSQC mirrors the engine's HUD mode numbers (touch_mode.qh from
touch_hudmode.h): every TOUCHMODE_*, TOUCHCTL_* and TOUCHDP_* constant the
QuakeC declares must have the C header's value."""
import re
import sys

c_src, qc_src = (open(p, encoding="utf-8").read() for p in sys.argv[1:3])
c = {}
for name, expr in re.findall(r"^#define\s+(TOUCH(?:MODE|CTL|DP)_\w+)\s+(.+?)\s*(?://.*)?$", c_src, re.M):
    try:
        c[name] = eval(expr.replace("f", "") if re.fullmatch(r"[\d.()<< f]+", expr) else expr, {}, {})
    except Exception:
        pass
qc = {name: float(v) for name, v in re.findall(r"^const (?:int|float) (TOUCH(?:MODE|CTL|DP)_\w+)\s*=\s*([\d.]+);", qc_src, re.M)}
bad = [f"{n}: QuakeC {v}, C {c.get(n)}" for n, v in qc.items() if n not in c or float(c[n]) != v]
missing = [n for n in c if (n.startswith("TOUCHCTL_") and "(" not in str(n)) and n not in qc
           and n not in ("TOUCHCTL_CHROME", "TOUCHCTL_PLAY")]
if bad or missing:
    for line in bad:
        print("MISMATCH", line)
    for n in missing:
        print("MISSING in touch_mode.qh:", n)
    sys.exit(1)
print(f"touch_mode.qh mirrors touch_hudmode.h: {len(qc)} constants")
