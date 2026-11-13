#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the hand-authored ("seed") UI bridge fixtures.

These are what native renderers develop against before (and alongside) the snapshots the
engine produces itself. Ids, props and sources follow docs/UI_BRIDGE.md and the ids the
QuakeC screens use. Run from anywhere:  python3 tests/uibridge/seed_fixtures.py
Only fixtures that the engine cannot regenerate keep "generated": "seed" for long
(scripted sequences such as a download); the rest are replaced by engine dumps.
"""
import copy, json, os, random

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "fixtures")

MAPS = [
    ('afterslime', 'Afterslime', 'kuniu the frogg, t0uYK8Ne', 'Medium sized indoor deathmatch map', 'dm tdm lms ca ft ka kh dom', '2-8', 'medium'),
    ('atelier', 'Atelier', 'ShadoW, Mirio, Debugger', 'Large map, well suited for team games', 'dm lms tdm ca ft ka kh dom', '6-16', 'large'),
    ('boil', 'Boil', 'kuniu the frogg, Mirio', 'Stay calm and watch how your enemy is boiling with anger.', 'dm tdm lms ca ft kh ka duel', '2-6', 'small'),
    ('catharsis', 'Catharsis', 'Justin, t0uYK8Ne', 'Medium-sized CTF level with crystals', 'ctf', '6-12', 'medium'),
    ('courtfun', 'Courtfun', 'SavageX, Calinou, Debugger', 'A classic large scale CTF map supporting other gamemodes as well', 'ctf cts ons nb', '8-16', 'large'),
    ('dance', 'Dance', 'FruitieX, dublpaws, Debugger, packer', 'Very open, small sized CTF map floating above an ocean', 'ctf kh ft ca', '4-10', 'small'),
    ('darkzone', 'Darkzone', 'Cortez, CuBe0wL, SavageX, Mossepo', 'Xonotic remake of a classic map from Quake1: Q1DM6. Recommended for cut-throat duels', 'dm lms ka tdm ca kh ft duel', '2-6', 'small'),
    ('erbium', 'Erbium', 'Justin, Mirio, t0uYK8Ne', 'Industrial, nature... Duel!', 'dm lms tdm ca ft ka kh dom duel', '2-6', 'small'),
    ('finalrage', 'Final Rage', 'Munyul Verminard, tZork, L0, proraide', 'Small map with brutal fast vertical action', 'dm tdm dom lms ca ft kh ka duel', '2-6', 'small'),
    ('fuse', 'Fuse', "Ferdinand 'cityy' List", 'Duel Mapping Contest Winner of 2012', 'dm lms ka kh ft ca tdm duel', '2-4', 'small'),
    ('geoplanetary', 'Geoplanetary', 'MintOX, Debugger, Mirio', 'Geomonetary in space', 'ctf dom', '4-10', 'medium'),
    ('glowplant', 'Glow Plant', 'tZork, Cuinnton, Debugger, Mirio', 'A nuclear power plant with a LOT of blue glowy stuff...', 'dm tdm dom lms ca kh ft ka', '4-12', 'medium'),
    ('implosion', 'Implosion', 'MintOX, Mirio, Mario, packer, SpiKe', 'Space Style CTF Level', 'ctf ft kh ka ca tdm lms dom dm', '6-12', 'medium'),
    ('runningman', 'Running Man', 'FruitieX, Cortez, Strahlemann, Debugger', 'Medium-sized, fast enclosed map', 'dm lms ka duel', '2-8', 'medium'),
    ('silentsiege', 'Silent Siege', 'Justin, kuniu the frogg', 'Medium size Duel/TDM map', 'dm lms tdm ca ft ka kh dom duel', '2-8', 'medium'),
    ('solarium', 'Solarium', 'theShadow, SpiKe', 'Medium sized open map, good for 3 to 6 players', 'dm tdm lms ca ka ft kh duel', '3-6', 'medium'),
    ('space-elevator', 'Space Elevator', 'FruitieX, Mirio, Cuinnton', 'Medium sized CTF space floater', 'ctf', '6-12', 'medium'),
    ('stormkeep', 'Stormkeep', 'FruitieX, Cuinnton, SavageX, Strahlemann, W. Libert, tZork, Mirio', 'Medium sized indoor map, well suited for team games', 'dm tdm dom lms kh ca ft ka duel', '4-12', 'medium'),
    ('techassault', 'Techassault', 'SavageX, Cortez', 'A medium-sized assault map', 'as', '8-16', 'medium'),
    ('vorix', 'Vorix', 'packer', 'This is Vorix!', 'ctf', '6-12', 'medium'),
    ('warfare', 'Warfare', 'Maddin, Tymo', 'Medium sized map with a central area. Well suited and optimized for duel matches.', 'dm tdm lms ca ka kh ft duel', '2-6', 'small'),
    ('xoylent', 'Xoylent', 'sev, Strahlemann', 'A remake of Soylent', 'dm lms tdm ca ft ka duel', '2-8', 'medium'),
]
MODES = [
    ("dm", "Deathmatch", "Score as many frags as you can", False),
    ("tdm", "Team Deathmatch", "Help your team score the most frags", True),
    ("ctf", "Capture the Flag", "Find and bring the enemy flag to your base", True),
    ("ca", "Clan Arena", "Kill all enemy players to win the round", True),
    ("duel", "Duel", "One on one", False),
    ("ft", "Freeze Tag", "Freeze the enemy team, thaw your teammates", True),
    ("dom", "Domination", "Capture and defend all the control points", True),
    ("ka", "Keepaway", "Hold the ball to score", False),
    ("kh", "Key Hunt", "Gather all the keys to win the round", True),
    ("lms", "Last Man Standing", "Survive and kill until the enemies have no lives left", False),
]
MODE_NAME = {m[0]: m[1] for m in MODES}


def N(id, type, children=(), origin=None, **props):
    n = {"id": id, "type": type, "props": props, "children": [c["id"] for c in children]}
    if origin:
        n["origin"] = origin
    return _wrap(n, children)


def _wrap(n, children):
    allnodes = [n]
    for c in children:
        allnodes += c["_all"]
    d = dict(n)
    d["_all"] = allnodes
    return d


def strip(n):
    return {k: v for k, v in n.items() if k != "_all"}


def A(id, label=None, icon=None, **kw):
    a = {"id": id}
    if label is not None:
        a["label"] = label
    if icon:
        a["icon"] = icon
    a.update(kw)
    return a


def app(screens, modal=(), context="menu", busy=None, toasts=()):
    stack = [s["id"] for s in screens]
    props = {"title": "Godnotic", "stack": stack, "modal": [m["id"] for m in modal],
             "canBack": len(stack) > 1 or bool(modal), "context": context}
    if busy:
        props["busy"] = busy
    return N("app", "app", list(screens) + list(modal) + list(toasts), **props)


def snapshot(root, rev=1):
    return {"uib": 1, "t": "snapshot", "id": 2, "rev": rev, "root": "app",
            "nodes": [strip(n) for n in root["_all"]]}


def fixture(name, desc, root, steps=(), rows=None, caps=None, rev=1):
    f = {"name": name, "description": desc, "generated": "seed", "snapshot": snapshot(root, rev)}
    if caps:
        f["caps"] = caps
    if steps:
        f["steps"] = list(steps)
    if rows:
        f["rows"] = rows
    return f


def mode_meta(modes):
    return " · ".join(MODE_NAME.get(m, m.upper()) for m in modes.split()[:3])

# ------------------------------------------------------------------ screens


def home(last="stormkeep", servers=None):
    m = next(x for x in MAPS if x[0] == last)
    tiles = N("home/tiles", "grid", [
        N("home/tiles/sp", "card", title="Singleplayer", subtitle=f"{m[1]} · Deathmatch · 4 bots",
          icon="singleplayer", image=f"pk3:maps/{last}", aspect="16:9", featured=True,
          primary=A("sp", "Open", "chevron")),
        N("home/tiles/campaign", "card", title="Campaign", subtitle="Level 3 of 31 · Capture the Flag: Implosion",
          icon="trophy", image="pk3:maps/implosion", aspect="16:9", featured=True, progress=2 / 31,
          primary=A("campaign", "Open", "chevron")),
        N("home/tiles/mp", "card", title="Multiplayer", subtitle=(servers or "Join a server, or create a game"),
          icon="multiplayer", image="pk3:maps/xoylent", aspect="16:9", featured=True, live=True,
          primary=A("mp", "Open", "chevron")),
    ], minItem=240, maxColumns=3)
    return N("home", "screen", [N("home/banner", "image", ref="asset:banner", role="banner", emphasis="high", aspect="21:9",
                                  fit="contain", alt="Godnotic"), tiles],
             title="Godnotic", kind="home", background="asset:background",
             actions=[A("settings", "Settings", "settings"), A("credits", "Credits", "credits"),
                      A("quit", "Quit", "quit", style="secondary",
                        confirm={"title": "Quit Godnotic?", "ok": "Quit"})])


CAMPAIGN = [("boil", "dm", "Deathmatch: Boil"), ("atelier", "dm", "Deathmatch: Atelier"), ("implosion", "ctf", "Capture the Flag: Implosion"),
            ("finalrage", "dm", "Deathmatch: Final Rage")]


def campaign_rows(index=2):
    rows = []
    for i, (mp, mode, title) in enumerate(CAMPAIGN[:index + 2]):
        state = "done" if i < index else "next" if i == index else "locked"
        rows.append({"key": str(i), "cells": {"n": i + 1, "title": f"{i + 1}. {title}", "map": mp, "mode": mode,
                                              "modeName": MODE_NAME.get(mode, mode), "state": state, "locked": i > index},
                     "image": f"pk3:maps/{mp}"})
    return rows


def campaign_screen(index=2, sel=None):
    sel = index if sel is None else sel
    mp, mode, title = CAMPAIGN[sel]
    return N("campaign", "screen", [
        N("campaign/sel", "preview", title=f"{sel + 1}. {title}", subtitle=f"{MODE_NAME.get(mode, mode)} on {mp.capitalize()}",
          image=f"pk3:maps/{mp}", aspect="16:9", density="compact", body="Capture the enemy flag before they take yours.",
          badges=[{"icon": "flag", "text": "Next", "tone": "accent"}] if sel == index else [{"icon": "check", "text": "Won", "tone": "positive"}]),
        N("campaign/levels", "section", [
            N("campaign/levels/list", "list", source="campaign", display="cards", rowHeight="card", minItem=220, aspectItem="16:9",
              state="ready", count=index + 2, selection="single", selected=[str(sel)],
              columns=[{"key": "title", "type": "text", "label": "Level", "width": "fill", "priority": 1},
                       {"key": "modeName", "type": "text", "label": "Mode", "width": "m", "priority": 2},
                       {"key": "state", "type": "text", "label": "State", "width": "s", "priority": 3}],
              rowSelect=A("campaign.select", "Choose", "check"), rowPrimary=A("campaign.play", "Play", "play", style="primary")),
        ], title="Levels", subtitle=f"{index} of 31 won · tap to choose, double tap to play", sticky=True),
    ], title="Campaign", subtitle="Xonotic campaign", kind="page", header={"collapsible": True},
        primary=A("campaign.play", "Play", "play", style="primary"))


def sp_screen(mode="dm", create=False, lan=False, pw=False, chosen="stormkeep"):
    cm = next(x for x in MAPS if x[0] == chosen)
    play = A("play", "Host on LAN" if lan else "Play", "host" if lan else "play", style="primary")
    modecards = [N(f"sp/modes/grid/{m[0]}", "card", title=m[1], subtitle=m[2], image=f"pk3:gfx/menu/luma/gametype_{m[0]}",
                   aspect="1:1", selected=(m[0] == mode), primary=A(f"mode.{m[0]}", m[1]))
                 for m in MODES]
    maps = N("sp/maps/list", "list", source=f"maps:{mode}", display="cards", count=len([x for x in MAPS if mode in x[4].split()]),
             state="ready", selection="single", selected=[chosen], rowHeight="card", rows=2, scroll="horizontal", minItem=160, aspectItem="16:9",
             columns=[{"key": "title", "type": "text", "label": "Map", "priority": 1},
                      {"key": "players", "type": "text", "label": "Players", "priority": 2},
                      {"key": "size", "type": "size", "label": "Size", "priority": 3}],
             rowSelect=A("map.select", "Choose", "check"),
             rowPrimary=A("map.play", "Play", "play", style="primary"),
             rowActions=[A("map.preview", "Details", "info")],
             empty={"title": "No maps for this mode", "icon": "map"})
    opts = N("sp/options", "form", [
        N("sp/options/bots", "stepper", label="Bots", bind="vm:sp.bots", value=4, min=0, max=15, step=1, format="int", icon="bot"),
        N("sp/options/skill", "choice", label="Bot skill", bind="vm:sp.skill", value="4", style="segmented",
          options=[{"value": "1", "label": "Easy"}, {"value": "4", "label": "Normal"},
                   {"value": "7", "label": "Hard"}, {"value": "10", "label": "Godlike"}]),
        N("sp/options/timelimit", "stepper", label="Time limit", bind="cvar:timelimit_override", value=10, min=0, max=60, step=5, format="int", unit="min"),
        N("sp/options/fraglimit", "stepper", label="Frag limit", bind="cvar:fraglimit_override", value=30, min=0, max=200, step=10, format="int"),
        N("sp/options/mutator", "choice", label="Weapons", bind="vm:sp.weapons", value="normal", style="dropdown",
          options=[{"value": "normal", "label": "Normal"}, {"value": "instagib", "label": "InstaGib"},
                   {"value": "nix", "label": "NIX"}, {"value": "rockets", "label": "Rocket arena"}]),
    ] + ([] if not create else [
        N("sp/options/lan", "switch", label="LAN", style="checkbox", bind="vm:create.lan", value=lan, icon="lan",
          help=("Others on your Wi-Fi or VPN can find and join this game" if lan else "Off: a local game nobody else can see or join")),
        N("sp/options/slots", "stepper", label="Player slots", bind="cvar:menu_maxplayers", value=8, min=2, max=32, step=1, format="int", icon="players",
          hidden=not lan),
        N("sp/options/port", "stepper", label="Port", bind="cvar:port", value=26000, min=1024, max=65535, step=1, format="int",
          help="26000 is free", hidden=not lan),
        N("sp/options/pwon", "switch", label="Password", bind="vm:create.pwon", value=pw, icon=("lock" if pw else "unlock"),
          help=("Players need the password to join" if pw else "Anyone on your network can join"), hidden=not lan),
        N("sp/options/password", "field", label="Password", bind="vm:create.password", value="", kind="password",
          private=True, hasValue=pw, hidden=not (lan and pw)),
    ]), density="compact")
    recent = N("sp/recent/list", "list", source="recent", display="rows", count=3, state="ready",
               columns=[{"key": "map", "type": "map", "label": "Map"}, {"key": "mode", "type": "mode", "label": "Mode"},
                        {"key": "when", "type": "date", "label": "When", "priority": 3}],
               rowPrimary=A("recent.play", "Play again", "play"), density="compact")
    return N("sp", "screen", [
        N("sp/modes", "section", [N("sp/modes/grid", "grid", modecards, scroll="horizontal", minItem=140)], title="Game mode"),
        N("sp/maps", "section", [maps], title="Map", subtitle="Tap to choose, double tap to play", sticky=True),
        N("sp/options-s", "section", [opts], title="Match"),
        N("sp/recent", "section", [recent], title="Recent", collapsible=True, importance=3),
    ], title=("Create a game" if create else "Singleplayer"), kind="page",
       subtitle=("On your network: others can join" if lan else "Local game: only you and the bots"),
       primary=play,
       header={"collapsible": True})


def map_preview(name="stormkeep", mode="dm"):
    m = next(x for x in MAPS if x[0] == name)
    return N("sp.map", "screen", [
        N("sp.map/preview", "preview", title=m[1], subtitle=mode_meta(m[4]), image=f"pk3:maps/{name}", aspect="16:9",
          body=m[3], attrs=[{"label": "Author", "value": m[2], "icon": "credits"},
                            {"label": "Players", "value": m[5], "icon": "players"},
                            {"label": "Size", "value": m[6].capitalize(), "icon": "map"},
                            {"label": "Modes", "value": ", ".join(MODE_NAME.get(x, x.upper()) for x in m[4].split()), "icon": "mode"}],
          primary=A("map.play", f"Play {MODE_NAME[mode]}", "play", style="primary"),
          actions=[A("map.choose", "Use this map", "check")]),
    ], title=m[1], kind="detail")


SERVER_WORDS = ["Arena", "Frag Palace", "Pickup", "Duel Pit", "CTF Night", "Casual", "Instagib", "Rocket Rush",
                "Newbies", "Veterans", "Clan Wars", "Midnight", "Euro", "Oceania", "Americas", "Asia"]


def servers(n=40, seed=7):
    r = random.Random(seed)
    rows = []
    for i in range(n):
        m = r.choice(MAPS)
        mode = r.choice(m[4].split())
        mx = r.choice([8, 12, 16, 24, 32])
        pl = r.randint(0, mx)
        bots = r.randint(0, min(pl, 4))
        lan = i < 3
        addr = f"192.168.1.{20 + i}:26000" if lan else f"203.0.113.{10 + i}:{26000 + (i % 3)}"
        if i == 2:
            addr = "100.64.0.5:26000"
        name = (f"Godnotic LAN {i + 1}" if lan else f"{r.choice(SERVER_WORDS)} #{i}")
        rows.append({"key": addr, "cells": {
            "name": name, "map": m[0], "mode": mode, "players": pl, "max": mx, "bots": bots,
            "ping": r.randint(2, 30) if lan else r.randint(18, 320), "lock": r.random() < 0.15,
            "fav": i in (5, 9), "address": addr, "net": ("vpn" if i == 2 else "lan") if lan else "internet",
            "version": "0.8.6"}, "image": f"pk3:maps/{m[0]}"})
    return rows


SERVER_COLS = [
    {"key": "lock", "type": "lock", "label": "", "width": "xs", "priority": 2},
    {"key": "name", "type": "text", "label": "Server", "sortable": True, "width": "fill", "priority": 1},
    {"key": "map", "type": "map", "label": "Map", "sortable": True, "width": "m", "priority": 2},
    {"key": "mode", "type": "mode", "label": "Mode", "sortable": True, "width": "s", "priority": 2},
    {"key": "players", "type": "players", "label": "Players", "sortable": True, "width": "s", "align": "end", "priority": 1},
    {"key": "ping", "type": "ping", "label": "Ping", "sortable": True, "width": "s", "align": "end", "priority": 1},
]


def mp_screen(rows=None, state="ready", total=None, lan=False, chosen=None, address=""):
    rows = rows or servers()
    count = len([r for r in rows if not lan or r["cells"]["net"] != "internet"]) if total is None else total
    bar = N("mp/bar", "toolbar", [
        N("mp/bar/search", "field", label="Search", bind="vm:mp.search", value="", kind="search", placeholder="Search servers"),
        N("mp/bar/filters", "row", [
            N("mp/bar/filters/lan", "switch", label="LAN", style="checkbox", bind="vm:mp.lan", value=lan, icon="lan"),
            N("mp/bar/filters/empty", "switch", label="Empty", style="checkbox", bind="cvar:menu_slist_showempty", value=True),
            N("mp/bar/filters/full", "switch", label="Full", style="checkbox", bind="cvar:menu_slist_showfull", value=True),
            N("mp/bar/filters/laggy", "switch", label="Laggy", style="checkbox", bind="cvar:menu_slist_showlaggy", value=False),
            N("mp/bar/filters/refresh", "button", label="Refresh", action=A("refresh", "Refresh", "refresh"), style="secondary"),
        ], wrap=True),
    ])
    lst = N("mp/servers", "list", source="servers", columns=SERVER_COLS, sort="ping", desc=False, filter="",
            selection="single", selected=([chosen] if chosen else []), live=True,
            density="compact", display="table", state=state, count=count, rowHeight="compact",
            rowSelect=A("server.select", "Choose", "check"),
            rowPrimary=A("server.join", "Join", "play", style="primary"),
            empty=({"title": "No games on this network", "text": "Games hosted on your Wi-Fi or VPN show up here.", "icon": "lan"}
                   if lan else {"title": "No servers yet", "text": "Check your connection, then Refresh.", "icon": "server",
                                "action": A("refresh", "Refresh", "refresh")}),
            error={"title": "The master server did not answer", "action": A("refresh", "Try again", "refresh")})
    join = N("mp/join", "actionbar", [
        N("mp/join/address", "field", label="Address", bind="vm:mp.address", value=address, kind="address",
          placeholder="host:port or godnotic://join/...", submit=A("join", "Join", "play")),
        N("mp/join/create", "button", label="Create", action=A("create", "Create", "add"), style="secondary"),
        # the one Join: the typed address, else the chosen row; off until there is either
        N("mp/join/go", "button", label="Join", action=A("join", "Join", "play", style="primary"), style="primary",
          enabled=bool(chosen or address)),
    ], align="spread")
    return N("mp", "screen", [bar, lst, join], title="Multiplayer", subtitle="40 servers · 87 players", kind="page",
             header={"collapsible": True})


def password_dialog(server="Godnotic LAN 1"):
    return N("mp.password", "screen", [
        N("mp.password/text", "text", body=f"{server} needs a password."),
        N("mp.password/form", "form", [
            N("mp.password/form/pw", "field", label="Password", bind="vm:join.password", value="", kind="password",
              private=True, hasValue=False, submit=A("password.join", "Join")),
        ], submit=A("password.join", "Join", "play", style="primary")),
    ], title="Password", presentation="dialog", dismissable=True, primary=A("password.join", "Join", "play", style="primary"))


def settings_root():
    cats = [("video", "Video", "video", "Quality, field of view, frame rate"),
            ("audio", "Audio", "audio", "Volume, music, voice"),
            ("game", "Game", "game", "Crosshair, HUD, messages"),
            ("input", "Input", "input", "Mouse, keyboard and gamepad"),
            ("touch", "Touch controls", "touch", "Presets, HUD, aim assist, auto-shoot, tactics"),
            ("esp", "ESP", "esp", "Offline matches only"),
            ("player", "Player", "player", "Name, model, colours"),
            ("privacy", "Privacy", "privacy", "Statistics and identity"),
            ("updates", "Updates", "update", "Channel and checks"),
            ("data", "Data", "data", "Game data, downloads, cache"),
            ("menu", "Menu", "menu", "Size and layout"),
            ("mods", "Mods", "mods", "Client and server mods, pages from mods")]
    return N("settings", "screen", [
        N("settings/sections", "grid", [
            N(f"settings/sections/{k}", "card", title=t, subtitle=s, icon=i, density="compact",
              primary=A(f"settings.{k}", t, "chevron")) for k, t, i, s in cats], minItem=240),
        N("settings/advanced", "button", label="Advanced settings", action=A("settings.advanced", "Advanced settings", "more", style="link"),
          help="Every option, in the classic settings dialogs"),
    ], title="Settings", kind="page")


def settings_video(fov=100):
    return N("settings.video", "screen", [
        N("settings.video/quality", "section", [
            N("settings.video/quality/preset", "choice", label="Quality", bind="vm:video.preset", value="medium", style="segmented",
              options=[{"value": x, "label": x.capitalize()} for x in ("low", "medium", "high", "ultra")], apply="video"),
            N("settings.video/quality/scale", "slider", label="Resolution scale", bind="cvar:r_viewscale", value=1.0, min=0.5, max=1.0,
              step=0.05, format="percent"),
            N("settings.video/quality/maxfps", "choice", label="Frame rate limit", bind="cvar:cl_maxfps", value="60", style="segmented",
              options=[{"value": "30", "label": "30"}, {"value": "60", "label": "60"}, {"value": "90", "label": "90"},
                       {"value": "120", "label": "120"}, {"value": "0", "label": "Unlimited"}]),
            N("settings.video/quality/vsync", "switch", label="Vertical sync", bind="cvar:vid_vsync", value=True, apply="video"),
        ], title="Quality", sticky=True),
        N("settings.video/view", "section", [
            N("settings.video/view/fov", "slider", label="Field of view", bind="cvar:fov", value=fov, min=60, max=130, step=1, format="deg"),
            N("settings.video/view/gamma", "slider", label="Brightness", bind="cvar:v_contrast", value=1.0, min=0.5, max=3.0, step=0.05, format="float2"),
            N("settings.video/view/bob", "switch", label="View bobbing", bind="cvar:cl_bob", value=False),
        ], title="View", sticky=True),
    ], title="Video", kind="page")


def settings_mods():
    return N("settings.mods", "screen", [
        N("settings.mods/client", "section", [
            N("settings.mods/client/list", "list", source="mods.client", display="rows", density="compact", state="ready", count=2,
              columns=[{"key": "name", "type": "text", "label": "Mod", "width": "fill"},
                       {"key": "version", "type": "text", "label": "Version", "priority": 3},
                       {"key": "enabled", "type": "bool", "label": "On"}],
              rowPrimary=A("mod.open", "Details", "info"),
              empty={"title": "No mods yet", "text": "Install one from a file or a link.", "icon": "mods"}),
        ], title="Client mods", subtitle="Run in your game only"),
        N("settings.mods/server", "section", [
            N("settings.mods/server/list", "list", source="mods.server", display="rows", density="compact", state="ready", count=1,
              columns=[{"key": "name", "type": "text", "label": "Mod", "width": "fill"},
                       {"key": "enabled", "type": "bool", "label": "On"}],
              rowPrimary=A("mod.open", "Details", "info")),
        ], title="Server mods", subtitle="Run in games you host"),
        N("settings.mods/install", "row", [
            N("settings.mods/install/file", "button", label="Install from file",
              action=A("mods.install.file", "Install from file", "folder", kind="file", accept=["godnotic-mod", "zip", "pk3"]), style="tonal"),
            N("settings.mods/install/link", "button", label="Install from link",
              action=A("mods.install.link", "Install from link", "link"), style="tonal"),
            N("settings.mods/install/howto", "button", label="How to make a mod",
              action=A("mods.howto", "How to make a mod", "help", kind="link",
                       url="https://github.com/dixonSolutions/Godnotic/blob/main/docs/MODDING.md"), style="link"),
        ], wrap=True),
        N("settings.mods/pages", "section", [
            N("settings.mods/pages/radar", "card", title="Radar", subtitle="Settings page from the mod Radar", icon="mods",
              origin=None, primary=A("modpage.radar.main", "Open", "chevron")),
        ], title="Pages from mods"),
    ], title="Mods", kind="page")


def mod_page():
    o = "mod:radar"
    return N("mods.page.radar.main", "screen", [
        N("mods.page.radar.main/general", "section", [
            N("mods.page.radar.main/general/range", "slider", origin=o, label="Range", bind="mod:radar.range", value=2000, min=500, max=8000, step=100, format="int", unit="qu"),
            N("mods.page.radar.main/general/teammates", "switch", origin=o, label="Show teammates", bind="mod:radar.teammates", value=True),
            N("mods.page.radar.main/general/about", "text", origin=o, body="Radar draws a minimap with nearby players.", style="caption"),
        ], origin=o, title="General"),
    ], origin=o, title="Radar", subtitle="From the mod Radar", kind="page")


def pause_screen(hosting=False):
    host = [N("pause/host", "card", title="Hosting on your network", subtitle="Others join with one of these addresses",
              icon="host", accent="ember", density="compact",
              badges=[{"icon": "lock", "text": "Password set", "tone": "warning"}],
              meta=[{"icon": "wifi", "text": "Wi-Fi 192.168.1.23:26000"}, {"icon": "vpn", "text": "VPN 100.64.0.5:26000"}],
              primary=A("host.popup", "Addresses", "host"),
              actions=[A("host.copy", "Copy address", "copy", kind="copy", text="192.168.1.23:26000"),
                       A("host.share", "Share", "share", kind="share", text="godnotic://join/192.168.1.23:26000")])] if hosting else []
    return N("pause", "screen", host + [
        N("pause/match", "card", title="Stormkeep", subtitle="Deathmatch · 7:42 left · 3rd of 5", image="pk3:maps/stormkeep",
          aspect="21:9", meta=[{"icon": "trophy", "text": "12 frags"}, {"icon": "players", "text": "5 players"}]),
        N("pause/actions", "grid", [
            N("pause/actions/resume", "button", label="Resume", action=A("resume", "Resume", "resume", style="primary"), style="primary"),
            N("pause/actions/settings", "button", label="Settings", action=A("settings", "Settings", "settings"), style="secondary"),
            N("pause/actions/team", "button", label="Change team", action=A("team", "Change team", "players"), style="secondary", hidden=True),
            N("pause/actions/spectate", "button", label="Spectate", action=A("spectate", "Spectate", "esp"), style="secondary"),
            N("pause/actions/disconnect", "button", label=("End the game" if hosting else "Leave match"),
              action=A("disconnect", "End the game" if hosting else "Leave match", "disconnect", style="destructive",
                       confirm={"title": "End the game for everyone?" if hosting else "Leave the match?",
                                "ok": "End" if hosting else "Leave", "destructive": True}), style="destructive"),
        ], minItem=200),
    ], title="Paused", kind="game", presentation="fullscreen")


def lan_popup(players=None, autoclose=True, locked=True):
    """The host's popup: addresses with Copy, the join link, the lock, who is in, Close when someone joins."""
    def addr(i, label, icon, a):
        return N(f"lan.host/addresses/a{i}", "row", [
            N(f"lan.host/addresses/a{i}/text", "text", body=a, style="mono", selectable=True),
            N(f"lan.host/addresses/a{i}/copy", "button", label="Copy", style="secondary", action=A("host.copy", "Copy", "copy", kind="copy", text=a)),
        ], align="spread", label=label, icon=icon)
    link = "godnotic://join/192.168.1.23:26000"
    return N("lan.host", "screen", [
        N("lan.host/addresses", "section", [
            addr(0, "Wi-Fi", "wifi", "192.168.1.23:26000"),
            addr(1, "VPN (tailscale0)", "vpn", "100.64.0.5:26000"),
            N("lan.host/addresses/link", "row", [
                N("lan.host/addresses/link/text", "text", body=link, style="mono", selectable=True),
                N("lan.host/addresses/link/copy", "button", label="Copy", style="secondary", action=A("host.copylink", "Copy", "copy", kind="copy", text=link)),
                N("lan.host/addresses/link/share", "button", label="Share", style="secondary", action=A("host.share", "Share", "share", kind="share", text=link)),
            ], align="spread", label="Link", icon="link"),
        ], title="Addresses"),
        N("lan.host/lock", "text", body=("Password set: players need it to join" if locked else "No password: anyone on your network can join"),
          icon=("lock" if locked else "unlock"), tone=("warning" if locked else "neutral"), style="note"),
        N("lan.host/players", "text", body=(f"In the game: {players}" if players else "Nobody has joined yet"), icon="players", style="caption", live=True),
        N("lan.host/autoclose", "switch", label="Close when someone joins", bind="cvar:touch_lan_popup_autoclose", value=autoclose,
          help="This popup goes away by itself when the first player joins"),
    ], title="Your LAN game", subtitle="Others on your Wi-Fi or VPN join with one of these", presentation="dialog", dismissable=True,
        primary=A("host.close", "Close", "check", style="primary"))


def loading_screen(progress=0.0, stage="Downloading map", detail="", cancel=True):
    return N("loading", "screen", [
        N("loading/status", "status", icon="download", title="Joining Clan Wars #12", body=stage, progress=progress,
          primary=A("loading.cancel", "Cancel", "close", style="secondary"),
          live=True),
        N("loading/progress", "progress", label=stage, value=progress, detail=detail, live=True,
          cancel=A("loading.cancel", "Cancel", "close")),
    ], title="Loading", kind="loading", presentation="fullscreen", background="pk3:maps/stormkeep")


def credits_screen():
    return N("credits", "screen", [
        N("credits/godnotic", "section", [
            N("credits/godnotic/text", "text", body="Godnotic is a touch-first build of Xonotic for phones, tablets, Linux and the web."),
        ], title="Godnotic"),
        N("credits/xonotic", "section", [
            N("credits/xonotic/text", "text", body="Xonotic is made by the Xonotic team and its many contributors."),
            N("credits/xonotic/list", "button", label="Everyone who made Xonotic", action=A("credits.full", "Everyone who made Xonotic", "credits"), style="link"),
        ], title="Xonotic"),
        N("credits/licence", "section", [
            N("credits/licence/text", "text", body="The engine and game code are free software under the GNU General Public License, version 2 or later. Game data is under its authors' licences (see each pack's licence file).", style="caption"),
            N("credits/licence/link", "button", label="Source code",
              action=A("credits.source", "Source code", "link", kind="link", url="https://github.com/dixonSolutions/Godnotic"), style="link"),
        ], title="Licence"),
    ], title="Credits", kind="page")


def quit_dialog():
    return N("dialog.quit", "screen", [
        N("dialog.quit/text", "text", body="Your settings are saved."),
        N("dialog.quit/buttons", "row", [
            N("dialog.quit/buttons/cancel", "button", label="Cancel", action=A("dialog.cancel", "Cancel", kind="back"), style="secondary"),
            N("dialog.quit/buttons/ok", "button", label="Quit", action=A("quit.confirm", "Quit", "quit"), style="destructive"),
        ], align="end"),
    ], title="Quit Godnotic?", presentation="dialog", dismissable=True)


# ---------------------------------------------------------------- rows

def map_rows(mode=None):
    out = []
    for m in MAPS:
        if mode and mode not in m[4].split():
            continue
        out.append({"key": m[0], "image": f"pk3:maps/{m[0]}", "cells": {
            "name": m[0], "title": m[1], "author": m[2], "modes": m[4], "players": m[5], "size": m[6]}})
    return out


def mode_rows():
    return [{"key": m[0], "image": f"pk3:gfx/menu/luma/gametype_{m[0]}",
             "cells": {"id": m[0], "name": m[1], "help": m[2], "team": m[3]}} for m in MODES]


MOD_ROWS = {
    "mods.client": [
        {"key": "radar", "cells": {"id": "radar", "name": "Radar", "version": "1.2.0", "enabled": True, "state": "ok", "tier": "read"}},
        {"key": "killfeed-plus", "cells": {"id": "killfeed-plus", "name": "Killfeed+", "version": "0.4.1", "enabled": False, "state": "stopped", "tier": "read"}},
    ],
    "mods.server": [
        {"key": "lowgrav", "cells": {"id": "lowgrav", "name": "Low gravity", "version": "1.0.0", "enabled": True, "state": "ok", "tier": "write"}},
    ],
}
RECENT_ROWS = [
    {"key": "r1", "image": "pk3:maps/stormkeep", "cells": {"map": "stormkeep", "mode": "dm", "when": "2026-11-13", "result": "2nd of 5", "address": ""}},
    {"key": "r2", "image": "pk3:maps/dance", "cells": {"map": "dance", "mode": "ctf", "when": "2026-11-12", "result": "Won 3:1", "address": "192.168.1.20:26000"}},
    {"key": "r3", "image": "pk3:maps/boil", "cells": {"map": "boil", "mode": "duel", "when": "2026-11-10", "result": "Lost 8:10", "address": ""}},
]


def players_rows(n=6):
    r = random.Random(3)
    names = ["Ratnode", "Bot Discovery", "Bot Lion", "Bot Shadow", "Bot Hellfire", "Bot Mystery", "Bot Death"]
    return [{"key": f"p{i}", "cells": {"name": names[i], "score": r.randint(-2, 30), "ping": 0 if i else 28, "team": ""}}
            for i in range(n)]


CAPS_PHONE = {"nodes": [], "hints": [], "width": 844, "height": 390, "dpi": 420, "widthClass": "expanded",
              "heightClass": "compact", "input": ["touch"], "locale": "en", "reducedMotion": False, "powerSave": False,
              "thermal": "nominal", "insets": {"top": 0, "right": 32, "bottom": 0, "left": 32}}


def diff(base, set_nodes=(), dels=()):
    return {"uib": 1, "t": "diff", "id": base + 10, "base": base, "rev": base + 1,
            "set": [strip(n) for n in set_nodes], "del": list(dels)}


def own(tree, id):
    """The node with this id from a builder result (no children expansion)."""
    for n in tree["_all"]:
        if n["id"] == id:
            return n
    raise KeyError(id)


def write(name, f):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".json")
    # the engine writes most fixtures itself (scripts/uib-fixtures.py); seeds fill the rest
    if os.path.exists(path) and '"generated": "engine"' in open(path, encoding="utf-8").read():
        return
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as fh:
        json.dump(f, fh, indent=1, ensure_ascii=False)
        fh.write("\n")


def main():
    rows_all = servers()
    srows = {"servers": rows_all,
             "servers.lan": [r for r in rows_all if r["cells"]["net"] != "internet"],
             "servers.fav": [r for r in rows_all if r["cells"]["fav"]]}
    allrows = {**srows, "maps": map_rows(), "maps:dm": map_rows("dm"), "maps:ctf": map_rows("ctf"),
               "modes": mode_rows(), "recent": RECENT_ROWS, **MOD_ROWS,
               }

    h = home()
    write("home", fixture("home", "Home: the banner and exactly three cards (Singleplayer, Campaign, Multiplayer); Settings, Credits and Quit are header actions.",
                          app([h]), rows=None, caps=CAPS_PHONE))
    write("home-live", fixture("home-live", "Home with the server list known: the Multiplayer card says how many servers and players.",
                               app([home(servers="214 servers · 87 players")]), rows=None))
    write("campaign", fixture("campaign", "Campaign: the chosen level beside its picture, the levels as cards (won, next, locked).",
                              app([h, campaign_screen()]), rows={"campaign": campaign_rows()}))
    write("singleplayer", fixture("singleplayer", "Singleplayer: mode cards, map picker (cards over maps:dm), match options, recent.",
                                  app([h, sp_screen()]), rows={k: allrows[k] for k in ("maps:dm", "maps", "modes", "recent")}))
    write("map-preview", fixture("map-preview", "Map preview page pushed over Singleplayer.",
                                 app([h, sp_screen(), map_preview()]), rows={"maps:dm": allrows["maps:dm"]}))
    write("singleplayer-create-lan", fixture("singleplayer-create-lan", "Create with LAN ticked and a password: port and password appear inline, closed lock.",
                                             app([h, mp_screen(), sp_screen(create=True, lan=True, pw=True)]), rows={k: allrows[k] for k in ("maps:dm", "recent")}))
    write("servers", fixture("servers", "Servers: search, one row of checkboxes (LAN, Empty, Full, Laggy), Refresh, the dense list, address field, Join.",
                             app([h, mp_screen()]), rows=srows))
    write("servers-lan", fixture("servers-lan", "Servers with the LAN checkbox ticked (Wi-Fi and VPN rows).",
                                 app([h, mp_screen(lan=True)]), rows=srows))
    write("password-dialog", fixture("password-dialog", "Password prompt dialog.",
                                     app([h, mp_screen(lan=True)], modal=[password_dialog()]), rows=srows))
    write("settings", fixture("settings", "Settings root: twelve sections including Mods, and Advanced.", app([h, settings_root()])))
    write("settings-mods", fixture("settings-mods", "Settings > Mods: client and server mods, install, how-to link, mod pages.",
                                   app([h, settings_root(), settings_mods()]), rows=MOD_ROWS))
    write("mod-page", fixture("mod-page", "A page contributed by a mod (origin mod:radar).",
                              app([h, settings_root(), settings_mods(), mod_page()]), rows=MOD_ROWS))
    write("pause", fixture("pause", "In-game pause menu.", app([pause_screen()], context="game")))
    write("pause-hosting", fixture("pause-hosting", "Pause menu while hosting on LAN: compact card with labelled join addresses and lock state.",
                                   app([pause_screen(hosting=True)], context="game")))
    write("lan-popup", fixture("lan-popup", "The host's LAN popup over the pause menu: addresses with Copy, the join link, lock, players, Close when someone joins, Close.",
                               app([pause_screen(hosting=True)], modal=[lan_popup()], context="game")))
    write("credits", fixture("credits", "Credits and licence.", app([h, credits_screen()])))

    # --- sequences
    # 1. server list filling in
    base = app([h, mp_screen(rows=rows_all[:0], state="loading", total=0)])
    steps, rev = [], 1
    for count in (8, 25, 40):
        steps.append({"delay": 0.4, "msg": {"uib": 1, "t": "source", "id": 100 + count, "source": "servers", "rev": count, "total": count},
                      "note": f"{count} servers known"})
        lst = own(mp_screen(rows=rows_all, state="ready" if count == 40 else "loading", total=count), "mp/servers")
        steps.append({"delay": 0.0, "msg": diff(rev, [lst])})
        rev += 1
    write("seq-server-list", fixture("seq-server-list", "The server list fills in: source notices, then list total/state diffs. "
                                     "Rows for any window come from rows.servers (first N by total).",
                                     base, steps=steps, rows=srows))
    # 2. download progressing
    lb = app([loading_screen(0.0, "Connecting", "")], context="loading",
             busy={"label": "Joining Clan Wars #12", "progress": 0.0, "cancel": A("loading.cancel", "Cancel", "close")})
    steps, rev = [], 1
    total = 40.0
    for i, p in enumerate((0.05, 0.2, 0.45, 0.7, 0.95, 1.0)):
        got = total * p
        det = f"{got:.1f} MB of {total:.0f} MB · 3.2 MB/s · {int((total - got) / 3.2)} s left"
        ls = loading_screen(p, "Downloading map" if p < 1 else "Loading map", det)
        a = app([ls], context="loading", busy={"label": "Joining Clan Wars #12", "progress": p,
                                               "cancel": A("loading.cancel", "Cancel", "close")})
        steps.append({"delay": 0.5, "msg": diff(rev, [own(a, "app"), own(ls, "loading/status"), own(ls, "loading/progress")])})
        rev += 1
    write("seq-download", fixture("seq-download", "Connecting and downloading a map: progress, size, speed, time left.", lb, steps=steps))
    # 3. a setting changing
    sb = app([h, settings_root(), settings_video(100)])
    s2 = settings_video(110)
    # 4. a dialog opening and closing
    db = app([h])
    with_d = app([h], modal=[quit_dialog()])
    qd = quit_dialog()
    write("seq-dialog", fixture("seq-dialog", "The Quit dialog opens over Home, then is dismissed.", db, steps=[
        {"delay": 0.5, "msg": diff(1, [own(with_d, "app")] + [n for n in qd["_all"]])},
        {"delay": 1.0, "msg": diff(2, [own(db, "app")], [n["id"] for n in qd["_all"]])},
    ]))
    # 5. a screen push (Home -> Singleplayer) and back
    spn = sp_screen()
    pushed = app([h, spn])
    write("seq-navigate", fixture("seq-navigate", "Home, push Singleplayer, back to Home.", app([h]), steps=[
        {"delay": 0.5, "msg": diff(1, [own(pushed, "app")] + spn["_all"])},
        {"delay": 1.0, "msg": diff(2, [own(app([h]), "app")], [n["id"] for n in spn["_all"]])},
    ], rows={k: allrows[k] for k in ("maps:dm", "recent")}))
    # 5a. the map picker: one tap chooses a map (row/select), the chosen-map preview and the list follow
    s1, s2 = sp_screen(chosen="stormkeep"), sp_screen(chosen="xoylent")
    write("seq-map-select", fixture("seq-map-select", "Map picker (two rows of tiles, scrolling sideways): a tap on Xoylent sends row/select; the engine answers with the "
                                    "list's selected row. A double tap would send row/activate and start the match.", app([h, s1]), steps=[
        {"delay": 0.5, "msg": diff(1, [own(s2, "sp/maps/list")]), "note": "row select xoylent"},
    ], rows={k: allrows[k] for k in ("maps:dm", "recent")}))
    # 5a2. the server list: a tap chooses a server and the one Join comes on; a double tap would join
    m1, m2 = mp_screen(), mp_screen(chosen=rows_all[5]["key"])
    write("seq-server-select", fixture("seq-server-select", "Multiplayer: a tap on a server sends row/select; the list shows it chosen and the one Join "
                                       "button is enabled. A double tap sends row/activate and joins.", app([h, m1]), steps=[
        {"delay": 0.5, "msg": diff(1, [own(m2, "mp/servers"), own(m2, "mp/join/go")]), "note": "row select"},
    ], rows=srows))
    # 5b. the LAN popup: a player joins and, with Close when someone joins on, the popup leaves with a toast
    ph = pause_screen(hosting=True)
    lp, lp2 = lan_popup(), lan_popup(players="you, Mara (23 ms)")
    tj = N("toast/1", "toast", text="A player joined your game", kind="success", icon="players", timeout=3)
    with_p = app([ph], modal=[lp], context="game")
    write("seq-lan-popup", fixture("seq-lan-popup", "Hosting on LAN: the popup shows who joined, then closes by itself (Close when someone joins) with a toast.",
                                   with_p, steps=[
        {"delay": 0.8, "msg": diff(1, [own(lp2, "lan.host/players")]), "note": "a player joined"},
        {"delay": 0.2, "msg": diff(2, [own(app([ph], context="game", toasts=[tj]), "app"), tj], [n["id"] for n in lp["_all"]]), "note": "popup closed"},
    ]))
    # 5c. the host ended the LAN game: back at Home with the reason
    te = N("toast/1", "toast", text="The host ended the game", kind="warning", icon="info", timeout=8)
    write("seq-host-ended", fixture("seq-host-ended", "A client's menu after the host of its LAN game left: Home and a toast saying why.",
                                    app([pause_screen()], context="game"), steps=[
        {"delay": 0.5, "msg": diff(1, [own(app([h], toasts=[te]), "app"), te] + h["_all"], [n["id"] for n in pause_screen()["_all"]])},
    ]))
    # 6. a toast
    t = N("toast/1", "toast", text="Server added to favourites", kind="success", icon="star_filled", timeout=3,
          action=A("toast.undo", "Undo"))
    write("seq-toast", fixture("seq-toast", "A toast appears and leaves.", app([h]), steps=[
        {"delay": 0.3, "msg": diff(1, [own(app([h], toasts=[t]), "app"), t])},
        {"delay": 3.0, "msg": diff(2, [own(app([h]), "app")], ["toast/1"])},
    ]))
    # strip helper keys
    for fn in os.listdir(OUT):
        p = os.path.join(OUT, fn)
        with open(p) as fh:
            s = fh.read()
        if '"_all"' in s:
            d = json.loads(s)

            def clean(x):
                if isinstance(x, dict):
                    return {k: clean(v) for k, v in x.items() if k != "_all"}
                if isinstance(x, list):
                    return [clean(v) for v in x]
                return x
            write(fn[:-5], clean(d))


if __name__ == "__main__":
    main()
