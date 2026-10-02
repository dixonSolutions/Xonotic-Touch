#!/usr/bin/env python3
"""Drive a game in the isolated headless gnome-shell (headless-shell.sh).

Touch and keys go through org.gnome.Mutter.RemoteDesktop, recordings through
org.gnome.Shell.Screencast, rotation through org.gnome.Mutter.DisplayConfig.
Set DBUS_SESSION_BUS_ADDRESS to the address headless-shell.sh printed.

    drive.py timeline.tl     run a timeline
    drive.py -               read timeline lines from stdin (keep a session open)

Timeline, one op per line, # comments. Coordinates are 0..1 of the screen.
    sleep <s>                         wait
    note <text>                       print a marker
    tap <x> <y>                       one finger down and up
    drag <x0> <y0> <x1> <y1> <ms>     one finger along a line
    fingerdown <slot> <x> <y>         multitouch: put finger <slot> down
    fingermove <slot> <x> <y>         move it
    fingerup <slot>                   lift it
    key <evdev-code>                  tap a key (41 = `, the console; 88 = F12)
    down <code> / up <code>           hold / release a key
    type <text>                       type US-layout text (letters, digits, space, - _ . ; :)
    shot                              F12, which the test autoexec binds to `screenshot`
    rec_start <file.webm>             start recording the screen
    rec_stop                          stop it (also stops when the driver exits)
    rotate <0|1|2|3>                  monitor transform: normal, 90, 180, 270
    wait_cmd <timeout-s> <shell cmd>  poll until the command exits 0

Mutter adds each virtual device on its first event and that event can be lost:
start a timeline with a warm-up (key 42, then a tap in a corner) and a pause.
"""
import subprocess
import sys
import time

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
RD = "org.gnome.Mutter.RemoteDesktop"
SC = "org.gnome.Mutter.ScreenCast"
DC = ("org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig", "org.gnome.Mutter.DisplayConfig")
CAST = ("org.gnome.Shell.Screencast", "/org/gnome/Shell/Screencast", "org.gnome.Shell.Screencast")


def call(name, path, iface, method, args=None, rtype=None):
    return bus.call_sync(name, path, iface, method, args,
                         GLib.VariantType(rtype) if rtype else None,
                         Gio.DBusCallFlags.NONE, -1, None)


class Session:
    """A RemoteDesktop session with a stream on the monitor. A monitor
    reconfiguration (rotation) ends the stream, so rotate() opens a new one."""

    def __init__(self):
        self.path = None
        self.open()

    def open(self):
        if self.path:
            try:
                call(RD, self.path, RD + ".Session", "Stop")
            except GLib.Error:
                pass
        monitors = call(*DC, "GetCurrentState").unpack()[1]
        self.connector = monitors[0][0][0]
        self.w, self.h = 1920, 1080
        for mode in monitors[0][1]:
            if mode[6].get("is-current"):
                self.w, self.h = mode[1], mode[2]
        logical = call(*DC, "GetCurrentState").unpack()[2]
        if logical and logical[0][3] in (1, 3):  # 90/270: the stream is portrait
            self.w, self.h = self.h, self.w
        self.path = call(RD, "/org/gnome/Mutter/RemoteDesktop", RD, "CreateSession", rtype="(o)").unpack()[0]
        sid = call(RD, self.path, "org.freedesktop.DBus.Properties", "Get",
                   GLib.Variant("(ss)", (RD + ".Session", "SessionId")), "(v)").unpack()[0]
        sc = call(SC, "/org/gnome/Mutter/ScreenCast", SC, "CreateSession",
                  GLib.Variant("(a{sv})", ({"remote-desktop-session-id": GLib.Variant("s", sid)},)),
                  "(o)").unpack()[0]
        self.stream = call(SC, sc, SC + ".Session", "RecordMonitor",
                           GLib.Variant("(sa{sv})", (self.connector, {})), "(o)").unpack()[0]
        call(RD, self.path, RD + ".Session", "Start")
        print(f"driver: session on {self.connector} {self.w}x{self.h}", flush=True)

    def notify(self, method, sig=None, *args):
        call(RD, self.path, RD + ".Session", method, GLib.Variant(sig, args) if sig else None)

    def key(self, code, down):
        self.notify("NotifyKeyboardKeycode", "(ub)", code, down)

    def tap_key(self, code):
        self.key(code, True)
        time.sleep(0.04)
        self.key(code, False)
        time.sleep(0.04)

    def finger(self, kind, slot, x=0.0, y=0.0):
        if kind == "up":
            self.notify("NotifyTouchUp", "(u)", slot)
        else:
            method = "NotifyTouchDown" if kind == "down" else "NotifyTouchMotion"
            self.notify(method, "(sudd)", self.stream, slot, x * self.w, y * self.h)


US = {**{c: 30 + i for i, c in enumerate("asdfghjkl")},
      **{c: 16 + i for i, c in enumerate("qwertyuiop")},
      **{c: 44 + i for i, c in enumerate("zxcvbnm")},
      **{c: 2 + i for i, c in enumerate("123456789")},
      "0": 11, " ": 57, "-": 12, ".": 52, ";": 39, "/": 53, "=": 13}
US_SHIFT = {"_": 12, ":": 39, '"': 40, "+": 13}


def run(lines):
    s = Session()
    for raw in lines:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        op, *a = line.split()
        if op == "sleep":
            time.sleep(float(a[0]))
        elif op == "note":
            print("driver:", " ".join(a), flush=True)
        elif op == "tap":
            s.finger("down", 0, float(a[0]), float(a[1]))
            time.sleep(0.08)
            s.finger("up", 0)
        elif op == "drag":
            x0, y0, x1, y1 = map(float, a[:4])
            steps = max(1, int(float(a[4]) / 16))
            s.finger("down", 0, x0, y0)
            for i in range(1, steps + 1):
                t = i / steps
                s.finger("motion", 0, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)
                time.sleep(float(a[4]) / 1000 / steps)
            s.finger("up", 0)
        elif op == "fingerdown":
            s.finger("down", int(a[0]), float(a[1]), float(a[2]))
        elif op == "fingermove":
            s.finger("motion", int(a[0]), float(a[1]), float(a[2]))
        elif op == "fingerup":
            s.finger("up", int(a[0]))
        elif op == "key":
            s.tap_key(int(a[0]))
        elif op == "down":
            s.key(int(a[0]), True)
        elif op == "up":
            s.key(int(a[0]), False)
        elif op == "type":
            for ch in line.split(None, 1)[1]:
                if ch in US_SHIFT:
                    s.key(42, True)
                    s.tap_key(US_SHIFT[ch])
                    s.key(42, False)
                elif ch in US:
                    s.tap_key(US[ch])
        elif op == "shot":
            s.tap_key(88)
            time.sleep(0.3)
        elif op == "rec_start":
            r = call(*CAST, "Screencast",
                     GLib.Variant("(sa{sv})", (a[0], {"framerate": GLib.Variant("u", 30),
                                                      "draw-cursor": GLib.Variant("b", False)})),
                     "(bs)").unpack()
            print("driver: recording", r[1] if r[0] else "FAILED", flush=True)
        elif op == "rec_stop":
            call(*CAST, "StopScreencast", rtype="(b)")
            print("driver: recording stopped", flush=True)
        elif op == "rotate":
            serial, monitors, _, _ = call(*DC, "GetCurrentState").unpack()
            mode = [m[0] for m in monitors[0][1] if m[6].get("is-current")][0]
            cfg = [(0, 0, 1.0, int(a[0]), True, [(monitors[0][0][0], mode, {})])]
            call(*DC, "ApplyMonitorsConfig",
                 GLib.Variant("(uua(iiduba(ssa{sv}))a{sv})", (serial, 1, cfg, {})))
            time.sleep(2)
            s.open()
        elif op == "wait_cmd":
            deadline = time.time() + float(a[0])
            cmd = " ".join(a[1:])
            ok = False
            while time.time() < deadline:
                if subprocess.call(cmd, shell=True, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL) == 0:
                    ok = True
                    break
                time.sleep(2)
            print("driver: wait_cmd", "ok" if ok else "TIMEOUT", cmd, flush=True)
        else:
            print("driver: unknown op", op, file=sys.stderr, flush=True)
    s.notify("Stop")
    print("driver: done", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    run(sys.stdin if sys.argv[1] == "-" else open(sys.argv[1]).read().splitlines())
