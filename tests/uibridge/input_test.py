#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The QuakeC renderer under scripted input (docs/UI_BRIDGE.md 11.1).

A headless engine with the default configuration (so this also checks that the
bridge menu IS the default menu) is driven through `uib_input`, which goes the
way a finger, a mouse and a key go (Key_Event and the window mouse position);
after every step the description is dumped and checked.

    UIB_BASEDIR=<basedir> [UIB_ENGINE=build/bin/xonotic] [UIB_LIBDIR=...] tests/uibridge/input_test.py [WxH] [touch|mouse] [shots dir]

Checks: a tap opens a screen; a drag over the map picker scrolls and chooses
nothing; one tap chooses a map and starts nothing; Escape goes back; typing in
the address field and Enter joins, Cancel on the loading screen returns to Home;
arrow keys and Enter open a card; a classic dialog opens over the menu, takes
Escape itself and leaves the bridge menu (not the old launcher) behind; a double
tap on a map starts the match and the pause menu's Resume and Leave work.
"""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
size = sys.argv[1] if len(sys.argv) > 1 else "1280x720"
mode = sys.argv[2] if len(sys.argv) > 2 else "mouse"
shots = sys.argv[3] if len(sys.argv) > 3 else ""
W, H = size.split("x")

steps = []          # (seconds to wait, command, name)


def step(name, cmd, wait=1.6):
    steps.append((wait, cmd, name))


step("start", "echo", 1.0)
step("tap-singleplayer", "uib_input tap home/tiles/sp")
step("drag-maps", "uib_input drag sp/maps/list#0 -260 0", 2.2)
step("tap-map", "uib_input tap sp/maps/list#7")
step("escape", "uib_input key ESCAPE")
step("tap-multiplayer", "uib_input tap home/tiles/mp")
step("tap-address", "uib_input tap mp/join/address")
step("type-address", "uib_input text 127.0.0.1:9")
step("enter-joins", "uib_input key ENTER", 14.0)
step("joinfail", "echo", 0.5)
step("joinfail-retry", "uib_input tap dialog.joinfail", 14.0)
step("joinfail-again", "echo", 0.5)
step("cancel-join", "uib_input key ESCAPE", 1.2)
step("home-again", "uib_input key ESCAPE", 1.0)
step("arrow", "uib_input key DOWNARROW", 0.6)
step("enter-opens", "uib_input key ENTER")
step("keys-in-list", "uib_input key DOWNARROW", 0.5)
step("keys-in-list2", "uib_input key DOWNARROW", 0.5)
step("keys-right", "uib_input key RIGHTARROW; uib_input key RIGHTARROW", 0.8)
step("escape2", "uib_input key ESCAPE")
step("open-settings", 'uib_event {\\"ev\\":\\"action\\",\\"node\\":\\"home\\",\\"action\\":\\"settings\\"}', 1.0)
step("open-classic", 'uib_event {\\"ev\\":\\"value\\",\\"node\\":\\"settings/split\\",\\"value\\":\\"menu\\"}', 1.0)
step("classic-up", "uib_input tap settings/split/menu/classic/advanced", 2.5)
step("classic-hold", "echo", 1.0)
step("classic-escape", "uib_input key ESCAPE", 2.0)
step("back-in-bridge", "uib_input key ESCAPE", 1.2)
step("tap-singleplayer2", "uib_input tap home/tiles/sp")
step("doubletap-map", "uib_input doubletap sp/maps/list#0", 22.0)
step("in-match", "togglemenu", 2.0)
step("resume", "uib_input tap pause/actions/resume", 1.5)
step("menu-again", "togglemenu", 2.0)
step("leave", 'uib_event {\\"ev\\":\\"action\\",\\"node\\":\\"pause/actions/disconnect\\",\\"action\\":\\"disconnect\\"}', 4.0)
step("end", "echo", 0.5)

ud = tempfile.mkdtemp(prefix="uib-input-")
os.makedirs(os.path.join(ud, "data"))
t = 6.0
lines = ["set _uib_r_state \"\"", "set _uib_r_debug 1", "set touch_lan_bots 1", "set touch_lan_map stormkeep", "set touch_lan_gametype dm"]
for i, (wait, cmd, name) in enumerate(steps):
    lines.append(f'defer {t:.2f} "{cmd}"')
    t += wait
    shot = f"; screenshot in_{i:02d}_{name}.png" if shots else ""
    lines.append(f'defer {t:.2f} "echo STEP {name}; echo RSTATE $_uib_r_state; uib_dump{shot}"'.replace("$_uib_r_state", "\\$_uib_r_state"))
    t += 0.4
lines.append(f'defer {t + 1:.2f} "quit"')
# $ in a deferred command is expanded when the defer line is read: read the state through a file instead
lines = [l.replace("echo RSTATE \\$_uib_r_state; ", "exec rstate.cfg; ") for l in lines]
open(os.path.join(ud, "data", "input.cfg"), "w").write("\n".join(lines) + "\n")
open(os.path.join(ud, "data", "rstate.cfg"), "w").write("echo RSTATE $_uib_r_state\n")

env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": ud, "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
       "LANG": "C"}
if os.environ.get("UIB_LIBDIR"):
    env["LD_LIBRARY_PATH"] = os.environ["UIB_LIBDIR"]
engine = os.environ.get("UIB_ENGINE", os.path.join(ROOT, "build", "bin", "xonotic"))
cmd = ["timeout", str(int(t + 60)), engine, "-xonotic", "-nosound", "-basedir", os.environ["UIB_BASEDIR"], "-userdir", ud,
       "+vid_width", W, "+vid_height", H, "+vid_fullscreen", "0", "+vid_touchscreen", "1" if mode == "touch" else "0",
       "+set", "menu_mouse_absolute", "1", "+set", "touch_firstrun_done", "1", "+set", "_touch_assets_ready", "1",
       # a player who has a name: no welcome dialog over the screens under test
       "+set", "touch_profile_done", "1",
       "+set", "net_slist_lan_only", "1", "+set", "cl_curl_enabled", "0", "+exec", "input.cfg"]
if subprocess.run(["unshare", "-rn", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
    cmd = ["unshare", "-rn"] + cmd
log = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=os.environ["UIB_BASEDIR"]).stdout.decode("utf-8", "replace")
if shots:
    os.makedirs(shots, exist_ok=True)
    for root, _, files in os.walk(ud):
        for f in files:
            if f.startswith("in_") and f.endswith(".png"):
                shutil.move(os.path.join(root, f), os.path.join(shots, f))
    open(os.path.join(shots, "input.log"), "w").write(log)

# the description after each step
state, cur = {}, None
for line in log.splitlines():
    if line.startswith("STEP "):
        cur = line[5:].strip()
        state[cur] = {"r": "", "nodes": {}}
    elif cur and line.startswith("RSTATE "):
        state[cur]["r"] = line[7:]
    elif cur and line.startswith('{"uib":1,"t":"snapshot"') and not state[cur]["nodes"]:
        try:
            state[cur]["nodes"] = {n["id"]: n for n in json.loads(line)["nodes"]}
        except ValueError:
            pass

fail = 0


def check(ok, what):
    global fail
    print(("ok   " if ok else "FAIL ") + what)
    fail |= not ok


def app(s):
    return state.get(s, {}).get("nodes", {}).get("app", {}).get("props", {})


def node(s, i):
    return state.get(s, {}).get("nodes", {}).get(i, {}).get("props", {})


check(len(state) == len(steps), f"every step ran ({len(state)}/{len(steps)})")
check(app("start").get("stack") == ["home"] and "top=home" in state.get("start", {}).get("r", ""),
      "with the default configuration the bridge menu draws Home (" + state.get("start", {}).get("r", "no renderer state")[:40] + ")")
check(app("tap-singleplayer").get("stack") == ["home", "sp"], "a tap on the Singleplayer card opens Singleplayer")
check(app("drag-maps").get("stack") == ["home", "sp"] and node("drag-maps", "sp/maps/list").get("selected") == ["stormkeep"],
      "a drag over the map picker scrolls it and chooses nothing")
sel = node("tap-map", "sp/maps/list").get("selected")
check(app("tap-map").get("stack") == ["home", "sp"] and sel and sel != ["stormkeep"] and app("tap-map").get("context") == "menu",
      f"one tap chooses a map ({sel}) and starts nothing")
check(app("escape").get("stack") == ["home"], "Escape goes back to Home")
check(app("tap-multiplayer").get("stack") == ["home", "mp"] and node("tap-multiplayer", "mp/join/go").get("enabled") is False,
      "Multiplayer opens with Join off")
check("127.0.0.1:9" in log.split("STEP type-address")[-1].split("STEP cancel-join")[0],
      "typing an address and Enter joins it (the engine was asked for 127.0.0.1:9)")
jf = node("joinfail", "dialog.joinfail/text").get("body", "")
check(app("joinfail").get("modal") == ["dialog.joinfail"] and "127.0.0.1" in jf and app("joinfail").get("context") == "menu",
      f"the join fails: the Could not join dialog says why ({jf!r}; {node('joinfail', 'dialog.joinfail/detail').get('body', '')[:60]!r})")
between = log.split("STEP joinfail\n")[-1].split("STEP joinfail-again")[0]
check("127.0.0.1:9" in between and app("joinfail-again").get("modal") == ["dialog.joinfail"],
      "Retry on the dialog joins the same address again, and the dialog comes back when that fails too")
check(app("cancel-join").get("modal") == [] and app("cancel-join").get("stack") == ["home", "mp"], "Escape closes the dialog; Multiplayer is under it")
check(app("home-again").get("stack") == ["home"] and app("home-again").get("context") == "menu" and not app("home-again").get("busy"),
      "after the join is given up the menu is back at Home, nothing left loading")
check(app("enter-opens").get("stack") == ["home", "sp"], f"Down and Enter open the first card ({app('enter-opens').get('stack')})")
ksel = node("keys-right", "sp/maps/list").get("selected")
check(ksel and ksel != node("enter-opens", "sp/maps/list").get("selected") and app("keys-right").get("stack") == ["home", "sp"],
      f"arrow keys reach the map picker and move its cursor, which chooses the map ({ksel})")
check(app("escape2").get("stack") == ["home"], "Escape closes it")
frames = lambda s: int((state.get(s, {}).get("r", "") + " frames=0").split("frames=")[1].split()[0])
check(app("classic-hold").get("stack") == ["home", "settings"] and frames("classic-hold") == frames("classic-up"),
      f"Advanced settings: the classic dialog draws and the bridge renderer stands aside ({frames('classic-hold') - frames('classic-up')} bridge frames in the next second)")
check(app("classic-escape").get("stack") == ["home", "settings"] and frames("classic-escape") > frames("classic-hold"),
      "Escape closes the classic dialog (not the screen under it) and the bridge menu draws again")
check(app("back-in-bridge").get("stack") == ["home"], "a second Escape goes back in the bridge menu")
check(app("in-match").get("context") == "game" and app("in-match").get("stack") == ["pause"], "a double tap on a map starts the match; the menu key shows Pause")
check(app("leave").get("context") == "menu" and app("leave").get("stack") == ["home"], "Leave match returns to Home")
print("input_test: all passed" if not fail else "input_test: FAILED")
sys.exit(1 if fail else 0)
