#!/usr/bin/env python3
"""Time the server list against the REAL master servers and public servers
(docs/NETWORK.md, "Measured"). Not part of the test suites: it needs the
internet, and its numbers depend on where it runs.

Each session is one engine start, offscreen, that opens the list 8 s after
the start (net_slist_open) and prints the stats of the first refresh that
settles. "cold" sessions start from an empty user directory (nothing saved,
the first open of a new install); "warm" sessions reuse one user directory
after a priming session, so the saved list is there.

  tests/srvlist/bench_live.py --engine ENGINE --basedir BASEDIR [--sessions 5]
      [--mode cold|warm] [--label NAME] [--set CVAR VALUE ...] [--cpus 0-5]

Traffic: one refresh is about 550-900 getstatus packets of 50 bytes and the
answers (some 100 kB); five sessions are well under a megabyte.
"""
import argparse, os, re, shutil, statistics, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser()
ap.add_argument("--engine", required=True)
ap.add_argument("--basedir", required=True)
ap.add_argument("--sessions", type=int, default=5)
ap.add_argument("--mode", choices=("cold", "warm"), default="cold")
ap.add_argument("--label", default="")
ap.add_argument("--set", nargs=2, action="append", default=[], metavar=("CVAR", "VALUE"))
ap.add_argument("--cpus", default="0-5")
ap.add_argument("--libs", default=os.path.expanduser("~/.cache/godnotic-slimtest/lib"))
ap.add_argument("--work", default="")
ap.add_argument("--quit", type=int, default=20, help="seconds after the start the engine quits (the list opens at 8)")
ap.add_argument("--gap", type=float, default=3.0, help="seconds between sessions (a server ignores a second getstatus within 1 s)")
a = ap.parse_args()

work = a.work or tempfile.mkdtemp(prefix="godnotic-slbench.")
os.makedirs(work, exist_ok=True)


def buf_errors(kind):
    """Linux: datagrams dropped because a socket buffer was full, kind Rcvbuf or Sndbuf (all sockets, v4 + v6)."""
    n = 0
    try:
        lines = open("/proc/net/snmp").read().splitlines()
        hdr, val = [l.split() for l in lines if l.startswith("Udp:")][:2]
        n += int(val[hdr.index(kind + "Errors")])
        n += sum(int(l.split()[1]) for l in open("/proc/net/snmp6") if l.startswith("Udp6%sErrors" % kind))
    except (OSError, ValueError, IndexError):
        return None
    return n


def socket_drops(pid):
    """Linux: datagrams the engine's own UDP sockets dropped (receive buffer full), from /proc/net/udp*."""
    inodes = set()
    try:
        for fd in os.listdir("/proc/%d/fd" % pid):
            try:
                t = os.readlink("/proc/%d/fd/%s" % (pid, fd))
            except OSError:
                continue
            if t.startswith("socket:["):
                inodes.add(t[8:-1])
        n = 0
        for path in ("/proc/%d/net/udp" % pid, "/proc/%d/net/udp6" % pid):
            for line in open(path).read().splitlines()[1:]:
                f = line.split()
                if len(f) > 12 and f[9] in inodes:
                    n += int(f[12])
        return n
    except (OSError, ValueError):
        return 0


def session(userdir, tag):
    os.makedirs(os.path.join(userdir, "data"), exist_ok=True)
    with open(os.path.join(userdir, "data", "autoexec.cfg"), "w") as f:
        f.write('seta touch_profile_done 1\nseta _cl_name "gntest"\nseta cl_allow_uid2name 0\n')
    with open(os.path.join(userdir, "data", "bench.cfg"), "w") as f:
        f.write('defer 8 "net_slist_open"\ndefer %d quit\n' % a.quit)
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy")
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    if a.libs:
        env["LD_LIBRARY_PATH"] = a.libs + ":" + env.get("LD_LIBRARY_PATH", "")
    cmd = ["taskset", "-c", a.cpus, a.engine, "-xonotic", "-basedir", a.basedir, "-userdir", userdir,
           "+vid_fullscreen", "0", "+vid_width", "640", "+vid_height", "360", "+set", "_touch_assets_ready", "1",
           "+set", "net_slist_stats_print", "1"]
    for k, v in a.set:
        cmd += ["+set", k, v]
    cmd += ["+exec", "bench.cfg"]
    log = os.path.join(work, tag + ".log")
    drops = buf_errors("Rcvbuf"), buf_errors("Sndbuf")
    own = 0
    with open(log, "wb") as f:
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, env=env)
        while p.poll() is None:
            own = max(own, socket_drops(p.pid))
            time.sleep(0.2)
    after = buf_errors("Rcvbuf"), buf_errors("Sndbuf")
    for line in open(log, errors="replace"):
        if line.startswith("SLIST ") and "still running" not in line:
            d = dict(re.findall(r"(\w+)=([\d./]+)", line), line=line.strip())
            if None not in drops + after:
                d["line"] += " machine_rcvbuf_drops=%d machine_sndbuf_drops=%d" % (after[0] - drops[0], after[1] - drops[1])
            d["line"] += " engine_socket_drops=%d" % own
            return d
    return None


rows = []
warm_ud = os.path.join(work, "warm-user")
if a.mode == "warm":
    shutil.rmtree(warm_ud, ignore_errors=True)
    session(warm_ud, "prime")
    subprocess.run(["sleep", str(a.gap)])
for i in range(a.sessions):
    ud = warm_ud if a.mode == "warm" else os.path.join(work, "cold-%d" % i)
    if a.mode == "cold":
        shutil.rmtree(ud, ignore_errors=True)
    s = session(ud, "%s-%d" % (a.mode, i))
    print(s["line"] if s else "session %d: no settled refresh" % i, flush=True)
    if s:
        rows.append(s)
    subprocess.run(["sleep", str(a.gap)])

if not rows:
    sys.exit(1)


def col(key):
    v = [float(r[key]) for r in rows if key in r]
    return "%.2f (%.2f-%.2f)" % (statistics.median(v), min(v), max(v)) if v else "-"


print("| %s | sessions | rows in the first frame | first row | t50 | t95 | settled | answers | listed | queries sent |" % (a.label or a.mode))
print("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s |" % (
    a.label or a.mode, len(rows), int(statistics.median(int(r["rows_at_open"]) for r in rows)),
    col("first_row"), col("t50"), col("t95"), col("settled"),
    int(statistics.median(int(r["answers"]) for r in rows)), int(statistics.median(int(r["listed"]) for r in rows)),
    int(statistics.median(int(r["sent"]) for r in rows))))
