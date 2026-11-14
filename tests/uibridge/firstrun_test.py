#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""First start through the default menu, on a touchscreen (docs/UI_BRIDGE.md).

    UIB_BASEDIR=<basedir> [UIB_ENGINE=build/bin/xonotic] [UIB_LIBDIR=...] tests/uibridge/firstrun_test.py [shots dir]

1. Welcome: a fresh install asks for a name; a tap on the field asks for the
   screen keyboard (vid_touchscreen_showkeyboard) and places it under the field;
   typing turns Continue on; Enter saves the name and the keyboard goes away.
2. Setup: while packaging/start.sh is still downloading the game data
   (_touch_asset_fetch_active, touch/asset-progress.txt) the menu is one
   "setup" screen with the download's progress, not Home; "Download in
   background" leaves the marker start.sh reads and becomes "Close window",
   which quits; an error offers Try again; a finished download asks start.sh to
   run the engine again (the relaunch marker) and quits.
"""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
shots = sys.argv[1] if len(sys.argv) > 1 else ""
fail = 0


def check(ok, what):
    global fail
    print(("ok   " if ok else "FAIL ") + what)
    fail |= not ok


def run(name, steps, progress=None, extra=(), secs=40):
    """Runs a fresh offscreen engine with a touchscreen; steps are (time, command, label).
    Returns (log, {label: {"nodes": snapshot nodes, "vars": {cvar: value}}}, files under the userdir)."""
    ud = tempfile.mkdtemp(prefix="uib-firstrun-")
    os.makedirs(os.path.join(ud, "data", "touch"))
    if progress is not None:
        open(os.path.join(ud, "data", "touch", "asset-progress.txt"), "w").write(progress)
    # cvars are read through a file: $ in a deferred command is expanded when the defer line is read
    open(os.path.join(ud, "data", "vars.cfg"), "w").write(
        "echo VARS osk=$vid_touchscreen_showkeyboard tix=$vid_touchscreen_textinput_x tiw=$vid_touchscreen_textinput_w "
        "done=$touch_profile_done bg=$_touch_asset_fetch_background\necho NAME $_cl_name\n")
    lines = []
    for t, cmd, label in steps:
        shot = f"; screenshot fr_{name}_{label}.png" if shots else ""
        lines.append(f'defer {t:.2f} "{cmd}"')
        lines.append(f'defer {t + 0.8:.2f} "echo STEP {label}; exec vars.cfg; uib_dump{shot}"')
    lines.append(f'defer {steps[-1][0] + 3:.2f} "echo STILL-RUNNING; quit"')
    open(os.path.join(ud, "data", "run.cfg"), "w").write("\n".join(lines) + "\n")
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": ud, "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy", "LANG": "C"}
    if os.environ.get("UIB_LIBDIR"):
        env["LD_LIBRARY_PATH"] = os.environ["UIB_LIBDIR"]
    engine = os.environ.get("UIB_ENGINE", os.path.join(ROOT, "build", "bin", "xonotic"))
    cmd = ["timeout", str(secs + 30), engine, "-xonotic", "-nosound", "-basedir", os.environ["UIB_BASEDIR"], "-userdir", ud,
           "+vid_width", "1600", "+vid_height", "740", "+vid_fullscreen", "0", "+vid_touchscreen", "1",
           "+set", "menu_mouse_absolute", "1", "+set", "net_slist_lan_only", "1", "+set", "cl_curl_enabled", "0"] + list(extra) + ["+exec", "run.cfg"]
    if subprocess.run(["unshare", "-rn", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        cmd = ["unshare", "-rn"] + cmd
    log = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=os.environ["UIB_BASEDIR"]).stdout.decode("utf-8", "replace")
    files = set()
    for root, _, fs in os.walk(ud):
        for f in fs:
            rel = os.path.relpath(os.path.join(root, f), ud)
            files.add(rel)
            if shots and f.startswith("fr_") and f.endswith(".png"):
                os.makedirs(shots, exist_ok=True)
                shutil.copy(os.path.join(root, f), os.path.join(shots, f))
    if shots:
        open(os.path.join(shots, f"firstrun-{name}.log"), "w").write(log)
    shutil.rmtree(ud, ignore_errors=True)
    state, cur = {}, None
    for line in log.splitlines():
        if line.startswith("STEP "):
            cur = line[5:].strip()
            state[cur] = {"nodes": {}, "vars": {}, "name": ""}
        elif cur and line.startswith("VARS "):
            state[cur]["vars"] = dict(kv.split("=", 1) for kv in line[5:].split() if "=" in kv)
        elif cur and line.startswith("NAME "):
            state[cur]["name"] = line[5:].strip()
        elif cur and line.startswith('{"uib":1,"t":"snapshot"') and not state[cur]["nodes"]:
            try:
                state[cur]["nodes"] = {n["id"]: n for n in json.loads(line)["nodes"]}
            except ValueError:
                pass
    return log, state, files


def props(state, label, node):
    return state.get(label, {}).get("nodes", {}).get(node, {}).get("props", {})


# ---- 1. the welcome, the keyboard
log, s, _ = run("welcome", [
    (5, "echo", "start"),
    (6, "uib_input tap welcome/form/name", "tapped"),
    (8, "uib_input text Rat", "typed"),
    (10, "uib_input key ENTER", "saved"),
])
app = props(s, "start", "app")
check(app.get("stack") == ["home"] and app.get("modal") == ["welcome"] and app.get("canBack") is False,
      "a fresh install opens on the welcome, which cannot be dismissed (%s %s)" % (app.get("stack"), app.get("modal")))
check(props(s, "start", "welcome").get("primary", {}).get("enabled") is False, "Continue is off while there is no name")
v = s.get("tapped", {}).get("vars", {})
check(v.get("osk") in ("1", "2") and float(v.get("tiw") or 0) > 0,
      "a tap on the Name field asks for the screen keyboard and places it under the field (showkeyboard %s, width %s)" % (v.get("osk"), v.get("tiw")))
check(props(s, "typed", "welcome").get("primary", {}).get("enabled") is True, "typing a name turns Continue on")
app = props(s, "saved", "app")
v = s.get("saved", {}).get("vars", {})
check(app.get("modal") == [] and s.get("saved", {}).get("name") == "Rat" and v.get("done") == "1",
      "Enter saves the name and closes the welcome (name %r, touch_profile_done %s)" % (s.get("saved", {}).get("name"), v.get("done")))
check(v.get("osk") == "0", "the screen keyboard goes away with the field (showkeyboard %s)" % v.get("osk"))

# ---- 2. game data still downloading
fetch = ["+set", "_touch_asset_fetch_active", "1", "+set", "_touch_assets_ready", "0", "+set", "touch_profile_done", "1", "+name", "Rat"]
log, s, files = run("setup", [
    (5, "echo", "start"),
    (6, "uib_input tap setup/status", "background"),
    (8, "uib_input tap setup/status", "close"),
], progress="running\n42\nDownloading maps (120 of 290 MB)\n", extra=fetch)
app, st = props(s, "start", "app"), props(s, "start", "setup/status")
check(app.get("stack") == ["setup"], "while the data downloads the menu is the setup screen, not Home (%s)" % app.get("stack"))
check(abs(st.get("progress", -1) - 0.42) < 1e-3 and st.get("body") == "Downloading maps (120 of 290 MB)"
      and st.get("primary", {}).get("id") == "setup.background",
      "it shows the download's progress and message, with Download in background (%s, %r)" % (st.get("progress"), st.get("body")))
st = props(s, "background", "setup/status")
check(st.get("primary", {}).get("id") == "setup.close" and s.get("background", {}).get("vars", {}).get("bg") == "1"
      and any(f.endswith("touch/background-fetch-request.txt") for f in files),
      "Download in background leaves start.sh its marker and becomes Close window")
check("STILL-RUNNING" not in log, "Close window quits")

log, s, _ = run("error", [(5, "echo", "start")], progress="error\n0\nNot enough free space\n", extra=fetch)
st = props(s, "start", "setup/status")
check(st.get("primary", {}).get("id") == "setup.retry" and st.get("body") == "Not enough free space" and "progress" not in st,
      "a failed download says why and offers Try again, with no progress bar (%r)" % st.get("body"))

log, s, files = run("done", [(5, "echo", "start"), (9, "echo", "later")], progress="done\n100\n\n", extra=fetch)
check(any(f.endswith("touch/relaunch-request.txt") for f in files) and "STILL-RUNNING" not in log,
      "a finished download asks start.sh to run the engine again and quits")

log, s, _ = run("ready", [(5, "echo", "start")], extra=["+set", "_touch_assets_ready", "1", "+set", "touch_profile_done", "1", "+name", "Rat"])
check(props(s, "start", "app").get("stack") == ["home"] and props(s, "start", "app").get("modal") == [],
      "with the data in place and a name, the menu opens on Home")

print("firstrun: all passed" if not fail else "firstrun: FAILED")
sys.exit(fail)
