#!/usr/bin/env python3
"""Xonotic Touch StatusNotifierItem tray (host-side).

Mirrors Potato Tomato: Show window, Quit, download status, recent maps.
Runs outside the Flatpak sandbox so AppIndicator + notify-send work on GNOME
with the AppIndicator extension.
"""
from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time

import gi

gi.require_version("Gtk", "3.0")
try:
    gi.require_version("AppIndicator3", "0.1")
    from gi.repository import AppIndicator3 as AppIndicator
except (ValueError, ImportError):
    gi.require_version("AyatanaAppIndicator3", "0.1")
    from gi.repository import AyatanaAppIndicator3 as AppIndicator

from gi.repository import GLib, Gtk

USER_BASE = os.environ.get(
    "XONOTIC_TOUCH_USER_BASE",
    os.path.expanduser("~/.local/share/xonotic-touch"),
)
USER_DATA = os.path.join(USER_BASE, "data")
PROGRESS = os.path.join(USER_DATA, "touch", "asset-progress.txt")
CMD_FILE = os.path.join(USER_DATA, "touch", "tray-cmd.txt")
PIDFILE = os.path.join(USER_BASE, "tray.pid")
LOCKFILE = os.path.join(USER_BASE, "tray.lock")
FETCHD_PID = os.path.join(USER_BASE, "fetchd.pid")
FLATPAK_ID = os.environ.get(
    "XONOTIC_TOUCH_FLATPAK_ID", "io.github.dixonSolutions.XonoticTouch"
)
ICON_NAME = os.environ.get("XONOTIC_TOUCH_TRAY_ICON", FLATPAK_ID)
MAX_RECENT = 3
_LOCK_FH = None


def log(msg: str) -> None:
    print(f"xonotic-touch-tray: {msg}", file=sys.stderr, flush=True)


def write_cmd(cmd: str) -> None:
    os.makedirs(os.path.dirname(CMD_FILE), exist_ok=True)
    path = CMD_FILE + ".tmp"
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(cmd.strip() + "\n")
    os.replace(path, CMD_FILE)


def read_progress() -> tuple[str, str, str]:
    try:
        with open(PROGRESS, encoding="utf-8", errors="replace") as fh:
            lines = [ln.rstrip("\n") for ln in fh.readlines()[:3]]
        while len(lines) < 3:
            lines.append("")
        return lines[0], lines[1], lines[2]
    except OSError:
        return "", "", ""


def fetchd_alive() -> bool:
    try:
        with open(FETCHD_PID, encoding="utf-8") as fh:
            pid = int(fh.read().strip())
        os.kill(pid, 0)
        return True
    except (OSError, ValueError):
        return False


def engine_running() -> bool:
    try:
        out = subprocess.check_output(["pgrep", "-af", "bin/xonotic"], text=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False
    for line in out.splitlines():
        if "xonotic-touch-tray" in line:
            continue
        if "bin/xonotic" in line:
            return True
    return False


def recent_maps() -> list[str]:
    cfg = os.path.join(USER_DATA, "config.cfg")
    home_cfg = os.path.expanduser("~/.xonotic/data/config.cfg")
    for path in (cfg, home_cfg):
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        m = re.search(r'^g_maplist_mostrecent\s+"(.*)"\s*$', text, re.M)
        if not m:
            m = re.search(r"^g_maplist_mostrecent\s+(\S+)", text, re.M)
        if not m:
            continue
        raw = m.group(1).strip().strip('"')
        maps = [p for p in raw.replace(",", " ").split() if p]
        return maps[:MAX_RECENT]
    return []


def open_app(extra: list[str] | None = None) -> None:
    write_cmd("show" if not extra else "show " + " ".join(extra))
    if engine_running():
        return
    env = os.environ.copy()
    if FLATPAK_ID and shutil_which("flatpak"):
        cmd = ["flatpak", "run", FLATPAK_ID]
        if extra:
            # Map name hint for start.sh / future +map support
            env["XONOTIC_TOUCH_TRAY_MAP"] = extra[0]
        subprocess.Popen(cmd, env=env, start_new_session=True)
        return
    start = os.environ.get("XONOTIC_TOUCH_APP_ROOT", "")
    script = os.path.join(start, "bin", "start.sh") if start else ""
    if script and os.access(script, os.X_OK):
        subprocess.Popen([script], env=env, start_new_session=True)


def shutil_which(name: str) -> str | None:
    from shutil import which

    return which(name)


def stop_fetchd() -> None:
    try:
        with open(FETCHD_PID, encoding="utf-8") as fh:
            pid = int(fh.read().strip())
        os.kill(pid, signal.SIGTERM)
        time.sleep(0.4)
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    except (OSError, ValueError):
        pass
    # Orphan curl writers under our fetch-tmp
    needle = os.path.join(USER_DATA, ".fetch-tmp")
    try:
        out = subprocess.check_output(["pgrep", "-af", "curl|aria2c|wget"], text=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        out = ""
    for line in out.splitlines():
        if needle not in line:
            continue
        try:
            pid = int(line.split(None, 1)[0])
            os.kill(pid, signal.SIGTERM)
        except (OSError, ValueError):
            pass


def quit_all(_w=None) -> None:
    write_cmd("quit")
    stop_fetchd()
    # Ask Flatpak/engine to die
    if FLATPAK_ID and shutil_which("flatpak"):
        subprocess.Popen(["flatpak", "kill", FLATPAK_ID], start_new_session=True)
    Gtk.main_quit()


class Tray:
    def __init__(self) -> None:
        os.makedirs(os.path.join(USER_DATA, "touch"), exist_ok=True)
        with open(PIDFILE, "w", encoding="utf-8") as fh:
            fh.write(str(os.getpid()))
        self.ind = AppIndicator.Indicator.new(
            "xonotic-touch-tray",
            ICON_NAME,
            AppIndicator.IndicatorCategory.APPLICATION_STATUS,
        )
        self.ind.set_status(AppIndicator.IndicatorStatus.ACTIVE)
        self.status_item = None
        self.recent_items: list[Gtk.MenuItem] = []
        self.menu = Gtk.Menu()
        self._build_menu()
        self.ind.set_menu(self.menu)
        GLib.timeout_add_seconds(2, self._tick)

    def _build_menu(self) -> None:
        header = Gtk.MenuItem(label="Xonotic Touch")
        header.set_sensitive(False)
        self.menu.append(header)

        self.status_item = Gtk.MenuItem(label="Status: idle")
        self.status_item.set_sensitive(False)
        self.menu.append(self.status_item)
        self.menu.append(Gtk.SeparatorMenuItem())

        recent_hdr = Gtk.MenuItem(label="Previous games")
        recent_hdr.set_sensitive(False)
        self.menu.append(recent_hdr)
        for _ in range(MAX_RECENT):
            item = Gtk.MenuItem(label="(none)")
            item.set_sensitive(False)
            item.connect("activate", self._on_recent)
            self.menu.append(item)
            self.recent_items.append(item)

        self.menu.append(Gtk.SeparatorMenuItem())
        show = Gtk.MenuItem(label="Show window")
        show.connect("activate", lambda *_: open_app())
        self.menu.append(show)

        close_w = Gtk.MenuItem(label="Close window")
        close_w.connect("activate", self._close_window)
        self.menu.append(close_w)

        self.menu.append(Gtk.SeparatorMenuItem())
        quit_item = Gtk.MenuItem(label="Quit Xonotic Touch")
        quit_item.connect("activate", quit_all)
        self.menu.append(quit_item)
        self.menu.show_all()

    def _on_recent(self, item: Gtk.MenuItem) -> None:
        label = item.get_label() or ""
        if not label or label.startswith("("):
            return
        open_app([label])

    def _close_window(self, *_args) -> None:
        write_cmd("close")
        if FLATPAK_ID and shutil_which("flatpak") and engine_running():
            # Soft close: kill only the engine sandbox instance; fetchd/tray stay.
            # flatpak kill is heavy-handed for multi-instance; prefer pkill binary.
            subprocess.Popen(
                ["pkill", "-f", f"{FLATPAK_ID}.*/bin/xonotic"],
                start_new_session=True,
            )

    def _tick(self) -> bool:
        status, pct, msg = read_progress()
        if fetchd_alive() or status in ("discover", "running"):
            label = f"Downloading {pct}%".strip()
            if msg:
                short = msg if len(msg) < 48 else msg[:45] + "…"
                label = f"{label}: {short}" if pct else short
            self.status_item.set_label(label)
        elif status == "paused":
            self.status_item.set_label("Download paused")
        elif status == "done" or os.path.isfile(os.path.join(USER_DATA, ".assets-ready")):
            self.status_item.set_label("Game data ready")
        elif engine_running():
            self.status_item.set_label("Running")
        else:
            self.status_item.set_label("In tray")

        maps = recent_maps()
        for i, item in enumerate(self.recent_items):
            if i < len(maps):
                item.set_label(maps[i])
                item.set_sensitive(True)
            else:
                item.set_label("(none)")
                item.set_sensitive(False)
        return True


def _acquire_singleton() -> bool:
    """Exclusive flock — pidfiles alone race when start.sh relaunches quickly."""
    global _LOCK_FH
    import fcntl

    os.makedirs(USER_BASE, exist_ok=True)
    _LOCK_FH = open(LOCKFILE, "w", encoding="utf-8")
    try:
        fcntl.flock(_LOCK_FH.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        log("already running (lock held)")
        _LOCK_FH.close()
        _LOCK_FH = None
        return False
    _LOCK_FH.write(str(os.getpid()))
    _LOCK_FH.flush()
    return True


def main() -> int:
    if not _acquire_singleton():
        return 0

    def _cleanup(*_a):
        try:
            os.remove(PIDFILE)
        except OSError:
            pass
        Gtk.main_quit()

    signal.signal(signal.SIGTERM, _cleanup)
    signal.signal(signal.SIGINT, _cleanup)
    Tray()
    log(f"started for {USER_BASE}")
    Gtk.main()
    try:
        os.remove(PIDFILE)
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
