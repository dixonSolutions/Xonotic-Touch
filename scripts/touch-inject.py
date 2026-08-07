#!/usr/bin/env python3
"""Inject real multi-finger touch events through a virtual uinput touchscreen.

The touch redesign has to be verified with genuinely simultaneous fingers
(look + move + fire + jump at once). Pointer-emulation tools such as ydotool can
only ever produce a single contact, which is why earlier test runs could not
reproduce the multitouch paths (docs/test-runs/2026-07-25-ultramarine).

This creates a MT protocol B device that the compositor treats like the built-in
panel, then plays a small timeline of per-finger actions.

Script format — one action per line, blank lines and `#` comments ignored:

    down <slot> <x> <y>     put finger <slot> down at normalized 0..1 coords
    move <slot> <x> <y>     move finger <slot> (absolute normalized coords)
    glide <slot> <x> <y> <ms>   interpolate finger <slot> there over <ms>
    up <slot>               lift finger <slot>
    sleep <ms>              wait
    sync                    force an event frame
    key <NAME>              tap a key, e.g. `key F12` to trigger a screenshot

Example (fire while turning and moving):

    down 0 0.12 0.78
    down 1 0.85 0.70
    glide 1 0.70 0.70 400
    down 2 0.88 0.90
    sleep 300
    up 2
    up 1
    up 0

Usage:
    touch-inject.py --width 2880 --height 1920 script.txt
    echo "down 0 .5 .5" | touch-inject.py -
"""
from __future__ import annotations

import argparse
import sys
import time

from evdev import UInput, AbsInfo, ecodes as e

MAX_SLOTS = 10
# Kernel needs a settle window after device creation or the compositor misses
# the first contacts of the timeline.
DEVICE_SETTLE_S = 1.0


def build_device(width: int, height: int) -> UInput:
    """Create a multitouch-capable virtual touchscreen."""
    abs_axis = AbsInfo(value=0, min=0, max=0, fuzz=0, flat=0, resolution=0)
    capabilities = {
        e.EV_ABS: [
            (e.ABS_X, AbsInfo(0, 0, width - 1, 0, 0, 0)),
            (e.ABS_Y, AbsInfo(0, 0, height - 1, 0, 0, 0)),
            (e.ABS_MT_SLOT, AbsInfo(0, 0, MAX_SLOTS - 1, 0, 0, 0)),
            (e.ABS_MT_TRACKING_ID, AbsInfo(0, 0, 65535, 0, 0, 0)),
            (e.ABS_MT_POSITION_X, AbsInfo(0, 0, width - 1, 0, 0, 0)),
            (e.ABS_MT_POSITION_Y, AbsInfo(0, 0, height - 1, 0, 0, 0)),
            (e.ABS_MT_TOUCH_MAJOR, abs_axis),
            (e.ABS_MT_TOUCH_MINOR, abs_axis),
        ],
        e.EV_KEY: [e.BTN_TOUCH],
    }
    ui = UInput(capabilities, name="XonoticTouch Test Panel", version=1,
                input_props=[e.INPUT_PROP_DIRECT])
    return ui


class Keyboard:
    """Virtual keyboard, mainly so tests can trigger the engine's own
    `screenshot` bind — compositor screencasts come back black while the game
    holds a fullscreen direct-scanout surface."""

    def __init__(self) -> None:
        # Codes above KEY_MAX (and the KEY_MAX/KEY_CNT aliases themselves) are
        # rejected by the kernel, so clamp to the standard keyboard range.
        keys = sorted({code for name, code in vars(e).items()
                       if name.startswith("KEY_")
                       and isinstance(code, int)
                       and 0 < code < e.KEY_MAX})
        self.ui = UInput({e.EV_KEY: keys},
                         name="XonoticTouch Test Keyboard", version=1)

    def tap(self, name: str) -> None:
        code = getattr(e, f"KEY_{name.upper()}", None)
        if code is None:
            raise ValueError(f"unknown key {name!r}")
        self.ui.write(e.EV_KEY, code, 1)
        self.ui.syn()
        time.sleep(0.04)
        self.ui.write(e.EV_KEY, code, 0)
        self.ui.syn()

    def close(self) -> None:
        self.ui.close()


class Panel:
    def __init__(self, ui: UInput, width: int, height: int, verbose: bool):
        self.ui = ui
        self.width = width
        self.height = height
        self.verbose = verbose
        self.next_tracking_id = 1
        self.slot_tracking: dict[int, int] = {}
        self.slot_pos: dict[int, tuple[int, int]] = {}
        self.current_slot: int | None = None

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"  {msg}", file=sys.stderr)

    def _to_px(self, nx: float, ny: float) -> tuple[int, int]:
        x = max(0, min(self.width - 1, int(round(nx * (self.width - 1)))))
        y = max(0, min(self.height - 1, int(round(ny * (self.height - 1)))))
        return x, y

    def _select_slot(self, slot: int) -> None:
        if self.current_slot != slot:
            self.ui.write(e.EV_ABS, e.ABS_MT_SLOT, slot)
            self.current_slot = slot

    def sync(self) -> None:
        self.ui.syn()

    def down(self, slot: int, nx: float, ny: float) -> None:
        x, y = self._to_px(nx, ny)
        self._select_slot(slot)
        tracking_id = self.next_tracking_id
        self.next_tracking_id += 1
        self.slot_tracking[slot] = tracking_id
        self.slot_pos[slot] = (x, y)
        self.ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, tracking_id)
        self.ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, x)
        self.ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, y)
        # Single-touch emulation keeps clients that ignore MT working.
        if len(self.slot_tracking) == 1:
            self.ui.write(e.EV_KEY, e.BTN_TOUCH, 1)
        self.ui.write(e.EV_ABS, e.ABS_X, x)
        self.ui.write(e.EV_ABS, e.ABS_Y, y)
        self.sync()
        self._log(f"down slot={slot} at {x},{y}")

    def move(self, slot: int, nx: float, ny: float) -> None:
        if slot not in self.slot_tracking:
            raise ValueError(f"move on slot {slot} which is not down")
        x, y = self._to_px(nx, ny)
        self._select_slot(slot)
        self.slot_pos[slot] = (x, y)
        self.ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, x)
        self.ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, y)
        if slot == min(self.slot_tracking):
            self.ui.write(e.EV_ABS, e.ABS_X, x)
            self.ui.write(e.EV_ABS, e.ABS_Y, y)
        self.sync()

    def glide(self, slot: int, nx: float, ny: float, ms: int) -> None:
        if slot not in self.slot_tracking:
            raise ValueError(f"glide on slot {slot} which is not down")
        x0, y0 = self.slot_pos[slot]
        nx0, ny0 = x0 / (self.width - 1), y0 / (self.height - 1)
        # ~120 Hz to match the panel's real reporting rate.
        steps = max(1, int(ms / 8))
        for i in range(1, steps + 1):
            t = i / steps
            self.move(slot, nx0 + (nx - nx0) * t, ny0 + (ny - ny0) * t)
            time.sleep(ms / 1000.0 / steps)
        self._log(f"glide slot={slot} -> {nx:.3f},{ny:.3f} over {ms}ms")

    def up(self, slot: int) -> None:
        if slot not in self.slot_tracking:
            return
        self._select_slot(slot)
        self.ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, -1)
        del self.slot_tracking[slot]
        self.slot_pos.pop(slot, None)
        if not self.slot_tracking:
            self.ui.write(e.EV_KEY, e.BTN_TOUCH, 0)
        self.sync()
        self._log(f"up slot={slot}")

    def release_all(self) -> None:
        for slot in list(self.slot_tracking):
            self.up(slot)


def run_script(panel: Panel, keyboard: Keyboard, lines: list[str]) -> None:
    for lineno, raw in enumerate(lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        cmd = parts[0].lower()
        try:
            if cmd == "down":
                panel.down(int(parts[1]), float(parts[2]), float(parts[3]))
            elif cmd == "move":
                panel.move(int(parts[1]), float(parts[2]), float(parts[3]))
            elif cmd == "glide":
                panel.glide(int(parts[1]), float(parts[2]), float(parts[3]),
                            int(parts[4]))
            elif cmd == "up":
                panel.up(int(parts[1]))
            elif cmd == "sleep":
                time.sleep(int(parts[1]) / 1000.0)
            elif cmd == "sync":
                panel.sync()
            elif cmd == "key":
                keyboard.tap(parts[1])
            else:
                raise ValueError(f"unknown command {cmd!r}")
        except (IndexError, ValueError) as exc:
            raise SystemExit(f"line {lineno}: {raw.strip()!r}: {exc}") from exc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script", help="script file, or - for stdin")
    ap.add_argument("--width", type=int, default=2880, help="panel width in px")
    ap.add_argument("--height", type=int, default=1920, help="panel height in px")
    ap.add_argument("--settle", type=float, default=DEVICE_SETTLE_S,
                    help="seconds to wait after creating the device")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    text = sys.stdin.read() if args.script == "-" else open(args.script).read()

    ui = build_device(args.width, args.height)
    panel = Panel(ui, args.width, args.height, args.verbose)
    keyboard = Keyboard()
    try:
        time.sleep(args.settle)
        run_script(panel, keyboard, text.splitlines())
    finally:
        panel.release_all()
        time.sleep(0.1)
        ui.close()
        keyboard.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
