#!/usr/bin/env python3
"""Write engine/darkplaces/touch_srvlist_seed.h: the public servers that answer
right now, compiled into the engine so that the very first server list a
player opens (nothing saved yet) asks them at once, before the masters have
answered, and asks them first (docs/NETWORK.md, "The seed").

Asks the masters Xonotic asks (getserversExt, IPv4 and IPv6) once, then every
listed server with one getinfo (a second one 1.2 s later to the silent ones),
and keeps the addresses that answered as Xonotic servers. Sends about one
packet per listed address and two to the silent ones: some 900 packets of
50 bytes, a few seconds.

  scripts/srvlist-seed.py [--out FILE] [--min N] [--check]

--min N   refuse to write fewer than N servers (default 100): a runner with no
          UDP, or a master outage, leaves the committed seed alone (exit 3).
--churn F rewrite only when more than this fraction of the servers came or went
          (default 0.1), so a monthly run does not churn over a few servers.
--check   print what would change, write nothing.
Exit 0 written (or unchanged), 3 too few answers, 2 bad arguments.
"""
import argparse, datetime, os, select, socket, struct, sys, time

MASTERS = [("master1.xonotic.org", 42863), ("master2.xonotic.org", 27950), ("master3.xonotic.org", 27950),
           ("master4.xonotic.org", 42863), ("dpm4.xonotic.xyz", 27777), ("dpm6.xonotic.xyz", 27777)]
GAME = b"Xonotic"
PROTOCOL = 3
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "engine", "darkplaces", "touch_srvlist_seed.h")
MAX_SEED = 400   # the engine reads at most this many (TOUCHSL_SEED_MAX)


def parse_list(body):
    out = []
    i = 0
    while i < len(body):
        c = body[i:i + 1]
        if c == b"\\" and i + 7 <= len(body):
            if body[i + 1:i + 4] == b"EOT":
                break
            ip = socket.inet_ntoa(body[i + 1:i + 5])
            port = struct.unpack(">H", body[i + 5:i + 7])[0]
            i += 7
            if port and ip != "0.0.0.0":
                out.append((socket.AF_INET, ip, port))
        elif c == b"/" and i + 19 <= len(body):
            ip = socket.inet_ntop(socket.AF_INET6, body[i + 1:i + 17])
            port = struct.unpack(">H", body[i + 17:i + 19])[0]
            i += 19
            if port:
                out.append((socket.AF_INET6, ip, port))
        else:
            break
    return out


def norm(ip):
    """The address as the engine writes it (netconn.c, the master list): IPv6 as
    eight hex groups without leading zeros and without ::, in brackets."""
    if ":" not in ip:
        return ip
    groups = struct.unpack(">8H", socket.inet_pton(socket.AF_INET6, ip))
    return "[%s]" % ":".join("%x" % g for g in groups)


def collect(wait_masters=2.5, wait_servers=2.5):
    socks = {}
    s4 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    socks[socket.AF_INET] = s4
    try:
        s6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        s6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        socks[socket.AF_INET6] = s6
    except OSError:
        pass
    for s in socks.values():
        s.setblocking(False)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        except OSError:
            pass
    query = b"\xff\xff\xff\xffgetserversExt " + GAME + b" %d empty full ipv4 ipv6" % PROTOCOL
    masters = 0
    for host, port in MASTERS:
        try:
            infos = socket.getaddrinfo(host, port, type=socket.SOCK_DGRAM)
        except OSError as e:
            print("master %s: %s" % (host, e), file=sys.stderr)
            continue
        for fam, _, _, _, sa in infos:
            if fam in socks:
                try:
                    socks[fam].sendto(query, sa)
                    masters += 1
                except OSError as e:
                    print("master %s %s: %s" % (host, sa[0], e), file=sys.stderr)
    listed = {}
    asked = {}
    answered = {}

    def ask(key):
        fam = listed[key]
        try:
            socks[fam].sendto(b"\xff\xff\xff\xffgetinfo seed", key)
        except OSError:
            pass
        asked[key] = asked.get(key, 0) + 1

    def pump(until):
        while time.time() < until:
            r, _, _ = select.select(list(socks.values()), [], [], 0.02)
            for s in r:
                try:
                    d, a = s.recvfrom(65536)
                except OSError:
                    continue
                key = (a[0], a[1])
                if d.startswith(b"\xff\xff\xff\xffgetserversExtResponse") or d.startswith(b"\xff\xff\xff\xffgetserversResponse"):
                    for fam, ip, port in parse_list(d[d.index(b"Response") + 8:]):
                        k = (ip, port)
                        if k not in listed and fam in socks:
                            listed[k] = fam
                            ask(k)   # asked as soon as a master names it
                elif d.startswith(b"\xff\xff\xff\xffinfoResponse\n") and key in listed:
                    kv = d[17:].decode("utf-8", "replace").split("\\")[1:]
                    info = dict(zip(kv[0::2], kv[1::2]))
                    if info.get("gamename", "") == GAME.decode() and info.get("protocol", "") == str(PROTOCOL):
                        answered[key] = info

    t0 = time.time()
    pump(t0 + wait_masters)
    for k in [k for k in listed if k not in answered]:
        ask(k)
    pump(time.time() + wait_servers)
    v6 = sum(1 for ip, _ in answered if ":" in ip)
    print("masters asked %d, servers listed %d, answered %d (%d IPv4, %d IPv6), packets to servers %d"
          % (masters, len(listed), len(answered), len(answered) - v6, v6, sum(asked.values())), file=sys.stderr)
    return answered, listed


def render(addresses, date):
    lines = ["// Generated by scripts/srvlist-seed.py on %s: %d public servers that answered." % (date, len(addresses)),
             "// Do not edit by hand: run the script (or the srvlist-seed workflow) again.",
             "// The first server list a player opens asks these at once (touch_srvlist.c, docs/NETWORK.md).",
             "#define TOUCHSL_SEED_DATE \"%s\"" % date,
             "static const char *const touchsl_seed[] =",
             "{"]
    lines += ['\t"%s",' % a for a in addresses]
    lines += ["\tNULL", "};", ""]
    return "\n".join(lines)


def existing(path):
    try:
        with open(path) as f:
            return [l.strip().strip('",') for l in f if l.startswith('\t"')]
    except OSError:
        return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--min", type=int, default=100)
    ap.add_argument("--churn", type=float, default=0.1)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    answered, listed = collect()
    if len(answered) < a.min:
        print("only %d servers answered (at least %d wanted): the seed is left as it is" % (len(answered), a.min), file=sys.stderr)
        return 3
    found = set("%s:%d" % (norm(ip), port) for ip, port in answered)
    old = existing(a.out)
    if not any(":" in ip for ip, _ in answered):
        # no IPv6 here (GitHub's runners have none): keep the IPv6 servers of the
        # last seed that the masters still list
        still = set("%s:%d" % (norm(ip), port) for ip, port in listed)
        kept = [x for x in old if x.startswith("[") and x in still]
        print("no IPv6 answers from here: %d IPv6 servers of the last seed kept" % len(kept), file=sys.stderr)
        found |= set(kept)
    # deterministic order (diffs stay small); the engine decides the order of asking
    addresses = sorted(found)[:MAX_SEED]
    added = sorted(set(addresses) - set(old))
    gone = sorted(set(old) - set(addresses))
    print("seed: %d servers, %d new, %d gone since the last one" % (len(addresses), len(added), len(gone)))
    if a.check:
        return 0
    if old and len(added) + len(gone) <= a.churn * len(old):
        print("unchanged (%d changes, at most %d wanted for a rewrite)" % (len(added) + len(gone), int(a.churn * len(old))))
        return 0
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    with open(a.out, "w") as f:
        f.write(render(addresses, date))
    print("wrote " + a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
