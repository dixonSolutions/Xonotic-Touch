#!/usr/bin/env python3
"""The fast server list end to end against a fake master and fake servers on
loopback (fakenet.py), with the real engine, offscreen. Run by
scripts/test-srvlist.sh.

  1 cold    a fresh user directory: nothing saved. Bounds on the time to the
            first row, to half and 95% of the answers, and to "settled";
            every live server listed despite 3% loss; the servers were asked
            politely (never twice within a second, hosts with several ports
            not at once, dead ones not hammered).
  2 warm    the same user directory again: the saved rows are up in the frame
            the list opens, before a single answer (marked stale, counted in
            the summary), then refreshed in place.
  3 changed 20 servers are gone and 10 are new: the gone ones are removed,
            the new ones appear, the summary follows, and the removed ones
            are not shown from the saved list again.
  4 stock   net_slist_fast 0, for comparison (printed, and the stock
            scheduler must still work).
  5 seed    a fresh user directory with a seed (net_slist_seed naming a
            file of addresses) and a master that answers only after 0.6 s:
            the seeded servers are asked at once and answer before the
            master does; then the next session, with a saved list, takes no
            seed.
  6 model   net_slist_fast 2, the model of Insanotic's scheduler, still runs
            and settles (printed).

Every session refreshes three times (the menu's own refresh at start, a list
opened at 8 s and again at 14 s), so the politeness counters also cover
refreshes that follow each other closely.
"""
import argparse, json, os, re, shutil, signal, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--engine", required=True)
ap.add_argument("--basedir", required=True)
ap.add_argument("--work", required=True)
ap.add_argument("--libs", default="")
ap.add_argument("--port", type=int, default=27960)
a = ap.parse_args()
HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0


def ok(msg):
    print("ok   " + msg)


def bad(msg):
    global fails
    fails += 1
    print("FAIL " + msg)


def check(cond, msg, detail=""):
    (ok if cond else bad)(msg + (" (" + detail + ")" if detail else ""))


def servers_a():
    out = []
    for i in range(160):
        shared = i >= 76 and i < 100          # 24 live servers on one address
        s = {"ip": "127.77.9.9" if shared else "127.77.%d.%d" % (i // 200 + 1, i % 200 + 1),
             "port": 26000 + (i - 76 if shared else 0), "alive": i < 100,
             "latency_ms": 15 + (i * 37) % 236, "humans": 3 if i % 10 == 0 else 0, "bots": 2 if i % 7 == 0 else 0,
             "max": 16, "map": "stormkeep", "name": "Fake %d" % i}
        out.append(s)
    return out


def write_state(servers, master_delay=0):
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"servers": servers, "hold_answers": 0, "master_delay": master_delay}, f)
    os.replace(tmp, STATE)
    time.sleep(0.4)   # fakenet rereads it within 50 ms


def report():
    net.send_signal(signal.SIGUSR1)
    time.sleep(0.3)
    with open(REPORT) as f:
        return json.load(f)


def run_engine(userdir, fast, name, seed="0", quit_at=None):
    """One session: the app's own refresh at start (if it makes one), a list opened at 8 s and again at
    14 s, where the saved rows, the summary and the bridge's source are read in the same frame, and
    all three once more at the end. Returns (stats lines, summaries, bridge windows, log)."""
    os.makedirs(os.path.join(userdir, "data"), exist_ok=True)
    with open(os.path.join(userdir, "data", "autoexec.cfg"), "w") as f:
        # the masters here, not on the command line: xonotic-common.cfg sets them after an early +set,
        # and the menu refreshes the list by itself soon after the start
        f.write('seta touch_profile_done 1\nseta _cl_name "sltest"\nseta cl_allow_uid2name 0\n'
                'sv_master1 "127.0.0.1:%d"\n' % a.port
                + "".join('%s ""\n' % m for m in ("sv_master2", "sv_master3", "sv_master4", "sv_masterextra1", "sv_masterextra2", "sv_masterextra3")))
    with open(os.path.join(userdir, "data", "sl_open.cfg"), "w") as f:
        f.write('net_slist_open\nnet_slist_stats\n'
                'echo SUMMARY open "$_slist_summary" servers=$_slist_servers players=$_slist_players saved=$_slist_saved refreshing=$_slist_refreshing\n'
                + windows(7))
    with open(os.path.join(userdir, "data", "sl_end.cfg"), "w") as f:
        f.write('net_slist_stats\n'
                'echo SUMMARY end "$_slist_summary" servers=$_slist_servers players=$_slist_players saved=$_slist_saved refreshing=$_slist_refreshing\n'
                + windows(8))
    with open(os.path.join(userdir, "data", "sl.cfg"), "w") as f:
        if quit_at:
            f.write('defer 8 "net_slist_open"\ndefer %d "exec sl_end.cfg"\ndefer %d quit\n' % (quit_at - 1, quit_at))
        elif fast:
            f.write('defer 8 "net_slist_open"\ndefer 14 "exec sl_open.cfg"\ndefer 22 "exec sl_end.cfg"\ndefer 23 quit\n')
        else:
            f.write('defer 8 "net_slist_open"\ndefer 30 "exec sl_end.cfg"\ndefer 31 quit\n')
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy")
    env.pop("DISPLAY", None); env.pop("WAYLAND_DISPLAY", None)
    if a.libs:
        env["LD_LIBRARY_PATH"] = a.libs + ":" + env.get("LD_LIBRARY_PATH", "")
    log = os.path.join(a.work, name + ".log")
    cmd = [a.engine, "-xonotic", "-basedir", a.basedir, "-userdir", userdir, "+vid_fullscreen", "0", "+vid_width", "640", "+vid_height", "360",
           "+set", "_touch_assets_ready", "1"]
    cmd += ["+set", "net_slist_fast", str(int(fast)), "+set", "net_slist_stats_print", "1", "+set", "net_slist_lan_repeats", "0",
            "+set", "net_slist_favorites", "", "+set", "net_slist_seed", seed, "+exec", "sl.cfg"]
    with open(log, "wb") as f:
        subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env, timeout=120)
    text = open(log, errors="replace").read()
    stats = [dict(re.findall(r"(\w+)=([\d./]+)", l), running="still running" in l, line=l)
             for l in text.splitlines() if l.startswith("SLIST ")]
    sums = dict((m.group(1), dict(text=m.group(2), **dict(re.findall(r"(\w+)=(\d+)", m.group(3)))))
                for m in re.finditer(r'^SUMMARY (\w+) "?([^"\n]*?)"? (servers=.*)$', text, re.M))
    rows = {}
    for l in text.splitlines():
        i = l.find('{"uib":1,"t":"rows"')
        if i >= 0:
            try:
                j = json.loads(l[i:])
            except ValueError:
                continue
            w = rows.setdefault(j["re"] // 100, {"rows": [], "total": j["total"], "rev": j["rev"]})
            w["rows"] += j["rows"]
            w["rev"] = max(w["rev"], j["rev"])
    return stats, sums, rows, log


def windows(tag):
    """The whole servers source in windows small enough for a console line (uib_send prints the reply)."""
    return "".join('uib_send {"uib":1,"t":"window","id":%d,"source":"servers.all","offset":%d,"limit":12}\n' % (tag * 100 + k, k * 12)
                   for k in range(16))


def f(stat, key):
    return float(stat.get(key, "nan"))


os.makedirs(a.work, exist_ok=True)
STATE = os.path.join(a.work, "state.json")
REPORT = os.path.join(a.work, "report.json")
A = servers_a()
with open(STATE, "w") as fh:
    json.dump({"servers": A, "hold_answers": 0}, fh)
net = subprocess.Popen([sys.executable, os.path.join(HERE, "fakenet.py"), "--state", STATE, "--report", REPORT,
                        "--master-port", str(a.port), "--loss", "0.03"])
try:
    time.sleep(1.0)
    ud = os.path.join(a.work, "user")
    shutil.rmtree(ud, ignore_errors=True)
    alive = sum(1 for s in A if s["alive"])

    # ---- 1 a fresh user directory
    write_state(A)
    stats, sums, rows, log = run_engine(ud, True, "1-cold")
    r = report()
    done = [s for s in stats if not s["running"]]
    opening = [s for s in stats if s["running"]]
    if not done:
        bad("cold: no refresh settled (log: %s)" % log)
    else:
        s = done[0]           # the first refresh of a fresh install: nothing saved
        print("     " + s["line"])
        check(int(s["rows_at_open"]) == 0, "cold: nothing saved, no rows at open")
        check(f(s, "first_row") < 0.6, "cold: first row within 0.6 s", "%.3f s" % f(s, "first_row"))
        check(f(s, "t50") < 1.0, "cold: half of the answers within 1.0 s", "%.3f s" % f(s, "t50"))
        check(f(s, "t95") < 2.5, "cold: 95% of the answers within 2.5 s", "%.3f s" % f(s, "t95"))
        check(f(s, "settled") < 6.0, "cold: settled within 6 s", "%.3f s" % f(s, "settled"))
        check(int(s["answers"]) >= alive - 1, "cold: every live server answered despite 3% loss", "%s of %d" % (s["answers"], alive))
        e = sums.get("end", {})
        check(abs(int(e.get("servers", -1)) - alive) <= 1, "cold: the summary counts the listed servers", str(e))
        check(abs(int(e.get("players", -1)) - sum(x["humans"] for x in A if x["alive"])) <= 3, "cold: and their human players", str(e))
        check(e.get("text", "").endswith("players") and "servers" in e.get("text", ""), "cold: the summary reads N servers · M players", e.get("text", ""))
    if opening:
        o = opening[-1]       # the list opened again in the same session
        check(int(o["rows_at_open"]) >= alive - 1 and f(o, "first_answer") == 0, "same session: opening the list again shows the rows at once", "%s rows" % o["rows_at_open"])
    check(r["dropped_flood"] == 0, "cold: no server was asked twice within a second, over three refreshes", "min gap %s s %s" % (r["min_gap_same_server"], r.get("flood_examples")))
    check(r["max_host_burst"] <= 16 + 10 + 1, "cold: one host with 24 ports gets at most 16 queries at once, then 100 a second", "%s within 100 ms" % r["max_host_burst"])
    budget = (alive + 2 * (len(A) - alive) + 25) + 2 * (len(A) + 25)
    check(r["queries"] <= budget, "cold: live servers one ask, unknown silent ones two, known silent ones one", "%d queries over three refreshes, at most %d" % (r["queries"], budget))
    check(r["first_query"] is not None and r["first_query"] < 0.3, "cold: servers are asked as soon as the master answers", "%s s after the master was asked" % r["first_query"])
    check(os.path.exists(os.path.join(ud, "data", "touch_serverlist_info.txt")), "cold: the list was saved")

    # ---- 2 the next session
    write_state(A)
    stats, sums, rows, log = run_engine(ud, True, "2-warm")
    r = report()
    opening = [s for s in stats if s["running"]]
    done = [s for s in stats if not s["running"]]
    if not opening or not done:
        bad("warm: missing stats (log: %s)" % log)
    else:
        first, o, s = done[0], opening[-1], done[-1]
        print("     " + first["line"])
        check(int(first["rows_at_open"]) >= alive - 1 and f(first, "first_row") <= 0.001,
              "warm: the first refresh of the session starts with the saved rows up (time to first row 0)", "%s rows" % first["rows_at_open"])
        check(f(first, "t95") < 2.5 and f(first, "settled") < 6.0, "warm: refreshed and settled in time", "t95 %.3f, settled %.3f" % (f(first, "t95"), f(first, "settled")))
        check(int(o["rows_at_open"]) >= alive - 1, "warm: opening the list: the rows are up in that frame", "%s rows" % o["rows_at_open"])
        check(f(o, "first_answer") == 0, "warm: before any server answered", "first answer %.3f" % f(o, "first_answer"))
        check(int(sums.get("open", {}).get("servers", 0)) >= alive - 1 and sums.get("open", {}).get("saved") == "1",
              "warm: the summary has them at once, flagged as saved", str(sums.get("open")))
        w = rows.get(7)
        stale = sum(1 for x in (w or {}).get("rows", []) if x["cells"].get("stale")) if w else -1
        check(w is not None and w["total"] >= alive - 1 and stale >= w["total"] - 12, "warm: the bridge's servers source has them, marked stale", "total %s, stale %d" % (w and w["total"], stale))
        check(w is not None and all("image" not in x for x in w["rows"]), "warm: server rows carry no image")
        e = rows.get(8)
        check(e is not None and e["total"] >= alive - 1 and not any(x["cells"].get("stale") for x in e["rows"]), "warm: after the refresh no row is stale")
        check(e is not None and w is not None and e["rev"] > w["rev"], "warm: the source's rev moved as the answers came", "%s -> %s" % (w and w["rev"], e and e["rev"]))
        check(sums.get("end", {}).get("saved") == "0", "warm: the summary is live after the refresh", str(sums.get("end")))
    check(r["dropped_flood"] == 0, "warm: no server was asked twice within a second", str(r.get("flood_examples")))
    check(r["queries"] <= 3 * (len(A) + 25), "warm: servers that failed last time get one ask", "%d queries over three refreshes" % r["queries"])

    # ---- 3 changed: 20 live servers gone, 10 new
    B = [dict(s) for s in A]
    for s in B[20:40]:
        s["alive"] = False
    for i in range(10):
        B.append({"ip": "127.77.3.%d" % (i + 1), "port": 26000, "alive": True, "latency_ms": 40 + i * 10, "humans": 1, "bots": 0,
                  "max": 8, "map": "newmap", "name": "New %d" % i})
    alive_b = sum(1 for s in B if s["alive"])
    write_state(B)
    stats, sums, rows, log = run_engine(ud, True, "3-changed")
    opening = [s for s in stats if s["running"]]
    done = [s for s in stats if not s["running"]]
    if not opening or not done:
        bad("changed: missing stats (log: %s)" % log)
    else:
        first, o = done[0], opening[-1]
        print("     " + first["line"])
        check(int(first["rows_at_open"]) >= alive - 1, "changed: the session opens with what was saved", first["rows_at_open"])
        check(19 <= int(first["removed"]) <= 22, "changed: the 20 servers that are gone were taken out", "removed %s" % first["removed"])
        w = rows.get(7)
        names = set(x["cells"]["name"] for x in w["rows"]) if w else set()
        check(abs(int(o["rows_at_open"]) - alive_b) <= 1, "changed: opening the list afterwards shows the live set at once", o["rows_at_open"])
        check(w is not None and not any(n in names for n in ("Fake %d" % i for i in range(20, 40))), "changed: no removed server among the saved rows")
        e = rows.get(8)
        names = set(x["cells"]["name"] for x in e["rows"]) if e else set()
        check(e is not None and abs(e["total"] - alive_b) <= 1, "changed: the list is the live servers", "%s rows, %d live" % (e and e["total"], alive_b))
        check(sum(1 for n in names if n.startswith("New ")) == 10, "changed: the 10 new servers are listed")
        check(not any(n in names for n in ("Fake %d" % i for i in range(20, 40))), "changed: none of the gone servers is listed")
        check(abs(int(sums.get("end", {}).get("servers", -1)) - alive_b) <= 1, "changed: the summary follows", str(sums.get("end")))

    # ---- 4 stock scheduler, for comparison
    write_state(A)
    stats, sums, rows, log = run_engine(os.path.join(a.work, "user-stock"), False, "4-stock")
    r = report()
    done = [s for s in stats if not s["running"]]
    if not done:
        bad("stock: net_slist_fast 0 never settled (log: %s)" % log)
    else:
        s = done[0]
        print("     " + s["line"])
        check("stock" in s["line"] and int(s["answers"]) >= alive - 3, "stock: net_slist_fast 0 is the stock scheduler and still lists the servers", "%s answers" % s["answers"])

    # ---- 5 the seed: a fresh install, a slow master
    seeded = [x for x in A if x["alive"]][:60] + [x for x in A if not x["alive"]][:5]
    ud5 = os.path.join(a.work, "user-seed")
    shutil.rmtree(ud5, ignore_errors=True)
    os.makedirs(os.path.join(ud5, "data"), exist_ok=True)
    with open(os.path.join(ud5, "data", "seed.txt"), "w") as fh:
        fh.write("# test seed\n" + "".join("%s:%d\n" % (x["ip"], x["port"]) for x in seeded))
    write_state(A, master_delay=0.6)
    stats, sums, rows, log = run_engine(ud5, True, "5-seed", seed="seed.txt", quit_at=16)
    r = report()
    done = [s for s in stats if not s["running"]]
    if not done:
        bad("seed: no refresh settled (log: %s)" % log)
    else:
        s = done[0]
        print("     " + s["line"])
        check(int(s.get("seeded", 0)) == len(seeded), "seed: every seeded address is taken", "seeded %s of %d" % (s.get("seeded"), len(seeded)))
        check(f(s, "first_row") < 0.5, "seed: the first row is up before the master has answered (0.6 s)", "%.3f s" % f(s, "first_row"))
        check(int(s["answers"]) >= alive - 1, "seed: and every live server is listed, seeded or not", "%s of %d" % (s["answers"], alive))
    check(r["dropped_flood"] == 0, "seed: no server was asked twice within a second", str(r.get("flood_examples")))
    stats, sums, rows, log = run_engine(ud5, True, "5-seed-next", seed="seed.txt", quit_at=16)
    done = [s for s in stats if not s["running"]]
    check(bool(done) and int(done[0].get("seeded", -1)) == 0 and int(done[0]["rows_at_open"]) >= alive - 1,
          "seed: the next session has the saved list and takes no seed", done[0]["line"] if done else log)

    # ---- 6 the model of Insanotic's scheduler (docs/NETWORK.md, "Measured")
    write_state(A)
    stats, sums, rows, log = run_engine(os.path.join(a.work, "user-model"), 2, "6-model", quit_at=24)
    done = [s for s in stats if not s["running"]]
    if not done:
        bad("model: net_slist_fast 2 never settled (log: %s)" % log)
    else:
        print("     " + done[0]["line"])
        check("insanotic-model" in done[0]["line"] and int(done[0]["answers"]) >= alive - 3 and int(done[0]["rows_at_open"]) == 0,
              "model: net_slist_fast 2 runs Insanotic's scheduler (nothing saved) and lists the servers", "%s answers" % done[0]["answers"])
finally:
    net.terminate()
    try:
        net.wait(timeout=5)
    except subprocess.TimeoutExpired:
        net.kill()

if fails:
    print("srvlist: %d check(s) failed (logs in %s)" % (fails, a.work))
    sys.exit(1)
print("srvlist end to end: all checks passed")
