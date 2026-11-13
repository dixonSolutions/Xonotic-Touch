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


def home(last="stormkeep"):
    m = next(x for x in MAPS if x[0] == last)
    hero = N("home/hero", "hero", title="Play", subtitle=f"Continue: {m[1]} · Deathmatch · 4 bots",
             image=f"pk3:maps/{last}", aspect="16:9", featured=True,
             meta=[{"icon": "map", "text": m[1]}, {"icon": "mode", "text": "Deathmatch"},
                   {"icon": "bot", "text": "4 bots"}],
             primary=A("play", "Play", "play", style="primary"),
             actions=[A("quick", "Quick match", "players"), A("sp", "Choose…", "map")])
    tiles = N("home/tiles", "grid", [
        N("home/tiles/sp", "card", title="Singleplayer", subtitle="Bots, your rules",
          icon="singleplayer", image="pk3:gfx/menu/luma/gametype_dm", aspect="16:9",
          primary=A("sp", "Open", "chevron")),
        N("home/tiles/mp", "card", title="Multiplayer", subtitle="Internet, LAN and hosting",
          icon="multiplayer", image="pk3:gfx/menu/luma/gametype_ctf", aspect="16:9",
          primary=A("mp", "Open", "chevron")),
        N("home/tiles/settings", "card", title="Settings", subtitle="Video, controls, Mods",
          icon="settings", primary=A("settings", "Open", "chevron")),
        N("home/tiles/credits", "card", title="Credits", subtitle="The people behind Xonotic and Godnotic",
          icon="credits", primary=A("credits", "Open", "chevron")),
    ], minItem=200)
    return N("home", "screen", [N("home/banner", "image", ref="asset:banner", aspect="21:9", fit="contain",
                                  alt="Godnotic"), hero, tiles],
             title="Godnotic", kind="home", background="asset:background",
             actions=[A("quit", "Quit", "quit", style="secondary",
                        confirm={"title": "Quit Godnotic?", "ok": "Quit"})])


def sp_screen(mode="dm"):
    modecards = [N(f"sp/modes/grid/{m[0]}", "card", title=m[1], subtitle=m[2], image=f"pk3:gfx/menu/luma/gametype_{m[0]}",
                   aspect="1:1", selected=(m[0] == mode), primary=A(f"mode.{m[0]}", m[1]))
                 for m in MODES]
    maps = N("sp/maps/list", "list", source=f"maps:{mode}", display="cards", count=len([x for x in MAPS if mode in x[4].split()]),
             state="ready", selection="single", selected=["stormkeep"], rowHeight="card", minItem=220, aspectItem="16:9",
             columns=[{"key": "title", "type": "text", "label": "Map", "priority": 1},
                      {"key": "players", "type": "text", "label": "Players", "priority": 2},
                      {"key": "size", "type": "size", "label": "Size", "priority": 3}],
             rowPrimary=A("map.preview", "Preview", "info"),
             rowActions=[A("map.play", "Play", "play")],
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
    ], density="compact")
    recent = N("sp/recent/list", "list", source="recent", display="rows", count=3, state="ready",
               columns=[{"key": "map", "type": "map", "label": "Map"}, {"key": "mode", "type": "mode", "label": "Mode"},
                        {"key": "when", "type": "date", "label": "When", "priority": 3}],
               rowPrimary=A("recent.play", "Play again", "play"), density="compact")
    return N("sp", "screen", [
        N("sp/modes", "section", [N("sp/modes/grid", "grid", modecards, scroll="horizontal", minItem=140)], title="Game mode"),
        N("sp/maps", "section", [maps], title="Map", subtitle="Tap a map to see it", sticky=True),
        N("sp/options-s", "section", [opts], title="Match"),
        N("sp/recent", "section", [recent], title="Recent", collapsible=True, importance=3),
    ], title="Singleplayer", kind="page", primary=A("play", "Play", "play", style="primary"),
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


def server_card(id, row):
    c = row["cells"]
    return N(id, "card", title=c["name"], subtitle=f"{MODE_NAME.get(c['mode'], c['mode'])} · {c['map']}",
             image=row["image"], aspect="16:9",
             badges=([{"icon": "lock", "text": "Password", "tone": "warning"}] if c["lock"] else []) +
                    ([{"icon": "star_filled", "text": "Favourite", "tone": "accent"}] if c["fav"] else []),
             meta=[{"icon": "players", "text": f"{c['players']}/{c['max']}"}, {"icon": "ping", "text": f"{c['ping']} ms"}],
             primary=A("server.open", "Details", "info"), actions=[A("server.join", "Join", "play", style="primary")])


def mp_screen(tab="internet", rows=None, state="ready", total=None, hosting=False):
    rows = rows or servers()
    feats = [r for r in rows if r["cells"]["fav"]]
    pane_children = {"internet": [], "lan": [], "host": []}
    if tab == "internet":
        pane_children["internet"] = [
            N("mp/tabs/internet/featured", "section", [
                N("mp/tabs/internet/featured/grid", "grid",
                  [server_card(f"mp/tabs/internet/featured/grid/{i}", r) for i, r in enumerate(feats)],
                  scroll="horizontal", minItem=260)], title="Favourites", importance=2),
            N("mp/tabs/internet/servers", "list", source="servers", columns=SERVER_COLS, sort="ping", desc=False,
              searchable=True, filter="", selection="none", density="compact", display="table", state=state,
              count=len(rows) if total is None else total, rowHeight="compact",
              rowPrimary=A("server.open", "Details", "info"),
              rowActions=[A("server.join", "Join", "play"), A("server.fav", "Favourite", "star")],
              empty={"title": "No servers yet", "text": "Pull to refresh, or check your connection.", "icon": "server",
                     "action": A("refresh", "Refresh", "refresh")},
              error={"title": "The master server did not answer", "action": A("refresh", "Try again", "refresh")}),
        ]
    elif tab == "lan":
        lanrows = [r for r in rows if r["cells"]["net"] != "internet"]
        pane_children["lan"] = [
            N("mp/tabs/lan/servers", "list", source="servers.lan", columns=SERVER_COLS, sort="ping", density="compact",
              display="table", state="ready", count=len(lanrows), rowHeight="compact", rowPrimary=A("server.open", "Details", "info"),
              rowActions=[A("server.join", "Join", "play")],
              empty={"title": "No games on this network", "text": "Games hosted on your Wi-Fi or VPN show up here.",
                     "icon": "lan", "action": A("tab.host", "Host a game", "host")}),
            N("mp/tabs/lan/note", "text", body="Looking on Wi-Fi and VPN networks. A game on a VPN is reached by its VPN address.",
              style="caption"),
        ]
    else:
        form = N("mp/tabs/host/form", "form", [
            N("mp/tabs/host/form/name", "field", label="Game name", bind="cvar:hostname", value="Ratnode's game", kind="name", maxLength=64),
            N("mp/tabs/host/form/map", "card", title="Stormkeep", subtitle="Tap to choose another map", image="pk3:maps/stormkeep",
              aspect="16:9", primary=A("host.map", "Choose map", "map")),
            N("mp/tabs/host/form/mode", "choice", label="Mode", bind="vm:host.mode", value="dm", style="dropdown",
              options=[{"value": m[0], "label": m[1]} for m in MODES]),
            N("mp/tabs/host/form/max", "stepper", label="Players", bind="cvar:menu_maxplayers", value=8, min=2, max=32, step=1, format="int"),
            N("mp/tabs/host/form/bots", "stepper", label="Bots", bind="vm:host.bots", value=0, min=0, max=15, step=1, format="int"),
            N("mp/tabs/host/form/port", "stepper", label="Port", bind="cvar:port", value=26000, min=1024, max=65535, step=1, format="int", importance=3),
            N("mp/tabs/host/form/password", "field", label="Password", bind="vm:host.password", value="", kind="password",
              private=True, hasValue=False, help="Players on your network need it to join. It is never sent over the network."),
        ], submit=A("host.start", "Host game", "host", style="primary"))
        kids = [form]
        if hosting:
            kids.insert(0, N("mp/tabs/host/status", "card", title="Hosting Stormkeep", subtitle="Deathmatch · 1/8 players",
                             icon="host", featured=True, accent="ember",
                             meta=[{"icon": "wifi", "text": "Wi-Fi 192.168.1.23:26000"},
                                   {"icon": "vpn", "text": "VPN 100.64.0.5:26000"}],
                             primary=A("host.share", "Share", "share", kind="share",
                                       text="godnotic://join/192.168.1.23:26000"),
                             actions=[A("host.copy", "Copy join string", "copy", kind="copy", text="192.168.1.23:26000"),
                                      A("host.stop", "Stop hosting", "close", style="destructive",
                                        confirm={"title": "Stop hosting?", "body": "Everyone in your game is disconnected.",
                                                 "ok": "Stop", "destructive": True})]))
        pane_children["host"] = kids
    tabs = N("mp/tabs", "tabs", [
        N("mp/tabs/internet", "pane", pane_children["internet"], key="internet", label="Internet", icon="internet"),
        N("mp/tabs/lan", "pane", pane_children["lan"], key="lan", label="LAN", icon="lan"),
        N("mp/tabs/host", "pane", pane_children["host"], key="host", label="Host", icon="host"),
    ], style="segmented", value=tab, bind="vm:mp.tab")
    return N("mp", "screen", [tabs], title="Multiplayer", kind="page", header={"collapsible": True},
             actions=[A("mp.filters", "Filters", "filter"), A("mp.join", "Join by address", "link"),
                      A("refresh", "Refresh", "refresh")])


def server_detail(row):
    c = row["cells"]
    return N("mp.server", "screen", [
        N("mp.server/preview", "preview", title=c["name"], subtitle=f"{MODE_NAME.get(c['mode'], c['mode'])} on {c['map']}",
          image=row["image"], aspect="16:9",
          attrs=[{"label": "Players", "value": f"{c['players']}/{c['max']} ({c['bots']} bots)", "icon": "players"},
                 {"label": "Ping", "value": f"{c['ping']} ms", "icon": "ping"},
                 {"label": "Address", "value": c["address"], "icon": "server"},
                 {"label": "Version", "value": c["version"], "icon": "info"}],
          badges=[{"icon": "lock", "text": "Password", "tone": "warning"}] if c["lock"] else [],
          primary=A("server.join", "Join", "play", style="primary"),
          actions=[A("server.fav", "Favourite", "star"), A("server.copy", "Copy address", "copy", kind="copy", text=c["address"])]),
        N("mp.server/players", "section", [
            N("mp.server/players/list", "list", source=f"players:{c['address']}", density="compact", display="table", count=c["players"],
              state="ready", columns=[{"key": "name", "type": "text", "label": "Player", "width": "fill"},
                                      {"key": "score", "type": "number", "label": "Score", "align": "end", "sortable": True},
                                      {"key": "ping", "type": "ping", "label": "Ping", "align": "end"}],
              empty={"title": "Nobody is playing", "icon": "players"})], title="Players", sticky=True),
        N("mp.server/bar", "actionbar", [
            N("mp.server/bar/fav", "button", label="Favourite", action=A("server.fav", "Favourite", "star"), style="secondary"),
            N("mp.server/bar/join", "button", label="Join", action=A("server.join", "Join", "play", style="primary"), style="primary"),
        ], align="end"),
    ], title=c["name"], kind="detail", header={"collapsible": True})


def join_sheet():
    return N("mp.join", "screen", [
        N("mp.join/form", "form", [
            N("mp.join/form/address", "field", label="Address or join link", bind="vm:join.address", value="", kind="address",
              placeholder="192.168.1.23:26000", submit=A("join.go", "Join", "play")),
        ], submit=A("join.go", "Join", "play", style="primary")),
    ], title="Join by address", presentation="sheet", dismissable=True)


def password_dialog(server="Godnotic LAN 1"):
    return N("mp.password", "screen", [
        N("mp.password/text", "text", body=f"{server} needs a password."),
        N("mp.password/form", "form", [
            N("mp.password/form/pw", "field", label="Password", bind="vm:join.password", value="", kind="password",
              private=True, hasValue=False, submit=A("password.join", "Join")),
        ], submit=A("password.join", "Join", "play", style="primary")),
    ], title="Password", presentation="dialog", dismissable=True)


def filters_sheet():
    return N("mp.filters", "screen", [
        N("mp.filters/form", "form", [
            N("mp.filters/form/mode", "choice", label="Mode", bind="vm:mp.filter.mode", value="", style="dropdown",
              options=[{"value": "", "label": "Any"}] + [{"value": m[0], "label": m[1]} for m in MODES]),
            N("mp.filters/form/empty", "switch", label="Hide empty servers", bind="cvar:menu_slist_showempty_inv", value=False),
            N("mp.filters/form/full", "switch", label="Hide full servers", bind="cvar:menu_slist_showfull_inv", value=False),
            N("mp.filters/form/locked", "switch", label="Hide servers with a password", bind="vm:mp.filter.nolock", value=False),
            N("mp.filters/form/ping", "slider", label="Highest ping", bind="vm:mp.filter.ping", value=300, min=50, max=1000, step=50,
              format="ms"),
        ]),
    ], title="Filters", presentation="sheet", dismissable=True, actions=[A("filters.reset", "Reset", "refresh")])


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


def pause_screen():
    return N("pause", "screen", [
        N("pause/match", "card", title="Stormkeep", subtitle="Deathmatch · 7:42 left · 3rd of 5", image="pk3:maps/stormkeep",
          aspect="21:9", meta=[{"icon": "trophy", "text": "12 frags"}, {"icon": "players", "text": "5 players"}]),
        N("pause/actions", "grid", [
            N("pause/actions/resume", "button", label="Resume", action=A("resume", "Resume", "resume", style="primary"), style="primary"),
            N("pause/actions/settings", "button", label="Settings", action=A("settings", "Settings", "settings"), style="secondary"),
            N("pause/actions/team", "button", label="Change team", action=A("team", "Change team", "players"), style="secondary", hidden=True),
            N("pause/actions/spectate", "button", label="Spectate", action=A("spectate", "Spectate", "esp"), style="secondary"),
            N("pause/actions/disconnect", "button", label="Leave match",
              action=A("disconnect", "Leave match", "disconnect", style="destructive",
                       confirm={"title": "Leave the match?", "ok": "Leave", "destructive": True}), style="destructive"),
        ], minItem=200),
    ], title="Paused", kind="game", presentation="fullscreen")


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
               "players:" + rows_all[5]["key"]: players_rows(rows_all[5]["cells"]["players"] if rows_all[5]["cells"]["players"] <= 7 else 6)}

    h = home()
    write("home", fixture("home", "Home: banner, hero Play card, four section cards, Quit.", app([h]), rows=None, caps=CAPS_PHONE))
    write("singleplayer", fixture("singleplayer", "Singleplayer: mode cards, map picker (cards over maps:dm), match options, recent.",
                                  app([h, sp_screen()]), rows={k: allrows[k] for k in ("maps:dm", "maps", "modes", "recent")}))
    write("map-preview", fixture("map-preview", "Map preview page pushed over Singleplayer.",
                                 app([h, sp_screen(), map_preview()]), rows={"maps:dm": allrows["maps:dm"]}))
    write("multiplayer-internet", fixture("multiplayer-internet", "Multiplayer, Internet tab: favourite cards and the dense server list.",
                                          app([h, mp_screen("internet")]), rows=srows))
    write("multiplayer-lan", fixture("multiplayer-lan", "Multiplayer, LAN tab (Wi-Fi and VPN rows).",
                                     app([h, mp_screen("lan")]), rows=srows))
    write("multiplayer-host", fixture("multiplayer-host", "Multiplayer, Host tab while hosting: status card with join addresses and share string.",
                                      app([h, mp_screen("host", hosting=True)]), rows=srows))
    write("server-detail", fixture("server-detail", "Server detail with map preview and player list.",
                                   app([h, mp_screen("internet"), server_detail(rows_all[5])]), rows=allrows))
    write("join-sheet", fixture("join-sheet", "Join by address sheet over Multiplayer.",
                                app([h, mp_screen("internet")], modal=[join_sheet()]), rows=srows))
    write("password-dialog", fixture("password-dialog", "Password prompt dialog.",
                                     app([h, mp_screen("lan")], modal=[password_dialog()]), rows=srows))
    write("filters-sheet", fixture("filters-sheet", "Server list filters sheet.",
                                   app([h, mp_screen("internet")], modal=[filters_sheet()]), rows=srows))
    write("settings", fixture("settings", "Settings root: twelve sections including Mods, and Advanced.", app([h, settings_root()])))
    write("settings-video", fixture("settings-video", "Settings > Video bound to real cvars.", app([h, settings_root(), settings_video()])))
    write("settings-mods", fixture("settings-mods", "Settings > Mods: client and server mods, install, how-to link, mod pages.",
                                   app([h, settings_root(), settings_mods()]), rows=MOD_ROWS))
    write("mod-page", fixture("mod-page", "A page contributed by a mod (origin mod:radar).",
                              app([h, settings_root(), settings_mods(), mod_page()]), rows=MOD_ROWS))
    write("pause", fixture("pause", "In-game pause menu.", app([pause_screen()], context="game")))
    write("credits", fixture("credits", "Credits and licence.", app([h, credits_screen()])))

    # --- sequences
    # 1. server list filling in
    base = app([h, mp_screen("internet", rows=rows_all[:0], state="loading", total=0)])
    steps, rev = [], 1
    for count in (8, 25, 40):
        steps.append({"delay": 0.4, "msg": {"uib": 1, "t": "source", "id": 100 + count, "source": "servers", "rev": count, "total": count},
                      "note": f"{count} servers known"})
        lst = own(mp_screen("internet", rows=rows_all, state="ready" if count == 40 else "loading", total=count), "mp/tabs/internet/servers")
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
    write("seq-setting", fixture("seq-setting", "Field of view changed (by the renderer, then the console): value diffs.",
                                 sb, steps=[{"delay": 0.5, "msg": diff(1, [own(s2, "settings.video/view/fov")]), "note": "fov 110"},
                                            {"delay": 0.5, "msg": diff(2, [own(settings_video(90), "settings.video/view/fov")]), "note": "fov 90 from console"}]))
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
